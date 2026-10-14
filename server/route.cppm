module;

#include <array>
#include <charconv>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module pylai:route;

import asio;
import jk;
import :http;

// ⚠️ 导出面 = **只给使用者**：声明 DSL（`Verb` / `Request` / 动词别名 / `Pattern` / `ParseError`）。
// 其余（树、建树器、匹配器、`FixedString` / `Segment` / `Capture`、注册与派发接线）
// 一律**不导出** —— 同模块的其他 TU（`route.cpp` / `server.cppm` / `session.cppm` / `route.checks.cpp`）照样看得见。
namespace pylai::route {

// 观测钩子：请求**进入 / 离开**那一刻各一次 —— 由应用实现，框架只负责在边界上叫它。
// ⚠️ Stage 1 只**接线**，不实现采集（分片计数 / 直方图属 Stage 3，见 plan §4.1.4）；默认无观察者。
// 为什么是钩子而不只是 `Context` 里的槽：槽是**载体**（这次请求的数据放哪），钩子是**边界** ——
// 只有槽的话，404 / 405 / OPTIONS 这些**没进 handler** 的请求就没人记。
export class Observer
{
public:
	virtual ~Observer() = default;

	virtual void on_request_begin(http::Context&) {}
	virtual void on_request_end(http::Context&) {}
};

// 每个出现过的动词**一处**类型擦除单元（树 + 具体类型的槽位表，定义在后半）。
struct VerbRoutes;

class Router
{
public:
	Router();
	~Router();

	Router(Router&&) noexcept = default;
	Router& operator=(Router&&) noexcept = default;
	Router(const Router&) = delete;
	Router& operator=(const Router&) = delete;

public:
	// 装上 `Builder::route_on` 声明出来的路由表。⚠️ 只该装一次 ——
	// `Builder::route_on` 只允许调一次（它返回的 builder 已经没有 route_on 了）。
	void set_routes(std::vector<VerbRoutes> routes);

public:
	// 观测钩子（可选）：接线点在 `dispatch` 的进入 / 离开。传 `nullptr` 取消。
	void set_observer(Observer* observer) noexcept;

public:
	// 命中 → handler（响应由 handler 的响应类型自己发到通道）；HEAD 没有显式 `Head<>` 时落到同路径的 GET（正文由响应类型按 HEAD 抑制）；
	// 路径存在但动词不符 → 405 + `Allow`；OPTIONS 没有显式 `Options<>` 时自动 204 + `Allow`；否则 404。
	// `ctx` 是这条事务的状态（报文数据 + 响应状态）；两个通道只能从连接层来。
	asio::awaitable<void> dispatch(
		http::Context& ctx, http::BodyReader& in, http::ResponseWriter& writer
	) const;

private:
	// 每个出现过的动词一条。
	std::vector<VerbRoutes> methods;
	Observer* observer_{nullptr};
};

// ═══ path pattern —— 编译期解析 ═══

// NTTP 载体；必须是 structural type（成员全 public、无虚函数、无 mutable）。
template <std::size_t n>
struct FixedString
{
public:
	char data[n]{};

public:
	constexpr FixedString() noexcept = default;

	constexpr FixedString(const char (&s)[n]) noexcept
	{
		for (std::size_t i = 0; i < n; ++i)
			data[i] = s[i];
	}

	constexpr FixedString(std::array<char, n> a) noexcept
	{
		for (std::size_t i = 0; i < n; ++i)
			data[i] = a[i];
	}

	constexpr std::string_view view() const noexcept { return {data, n - 1}; }

	// `[Pos, End)`。长度是**算出来**的而不是推导出来的，所以结果能当 `FixedString<M>` 的模板实参
	// （`string_view` 不行）；含尾部 NUL，所以尺寸是 `End - Pos + 1`。
	template <std::size_t pos, std::size_t end>
	[[nodiscard]] consteval auto substr() const noexcept -> FixedString<end - pos + 1>
	{
		static_assert(pos <= end, "substr: Pos > End");
		static_assert(end < n, "substr: End 越界");

		FixedString<end - pos + 1> out{};
		for (std::size_t i = 0; i < end - pos; ++i)
			out.data[i] = data[pos + i];
		return out;
	}

	friend constexpr bool operator==(const FixedString& a, const FixedString& b) noexcept
	{
		return a.view() == b.view();
	}
};

// 数一个字符在 `s` 里出现了几次。`consteval` ⇒ 结果必然能当数组的维度。
template <FixedString s, char c>
[[nodiscard]] consteval std::size_t count_of() noexcept
{
	std::size_t n = 0;
	for(const char ch : s.view())
		if(ch == c) ++n;
	return n;
}

enum class SegKind : std::uint8_t
{
	literal,   // 静态字面量
	param,     // {name}
	catch_all, // {name...}，或尾斜杠产生的匿名全捕获
	end_mark,  // {$}
};

struct Segment
{
	SegKind kind{SegKind::literal};
	std::string_view text{};  // literal=字面量；param/catch_all=名字；end_mark=空
	std::uint32_t pos{0};     // raw 中该段**前面那个 '/'** 的偏移
	std::uint32_t raw_len{0}; // raw 中该段自身的长度（不含前面的 '/'）
};

struct PatternView
{
	const Segment* data{nullptr};
	std::size_t size{0};
	std::string_view raw{}; // 原始 pattern，字符片段的 label 就切它（静态存储，零拷贝）
};

export enum class ParseError : std::uint8_t
{
	none = 0,
	must_start_with_slash,
	empty_segment,
	orphan_brace,
	empty_param_name,
	bad_param_name,
	catch_all_not_last,
	duplicate_param,
};

export [[nodiscard]] constexpr const char* to_string(ParseError e) noexcept
{
	switch (e)
	{
	case ParseError::none: return "none";
	case ParseError::must_start_with_slash: return "must_start_with_slash";
	case ParseError::empty_segment: return "empty_segment";
	case ParseError::orphan_brace: return "orphan_brace";
	case ParseError::empty_param_name: return "empty_param_name";
	case ParseError::bad_param_name: return "bad_param_name";
	case ParseError::catch_all_not_last: return "catch_all_not_last";
	case ParseError::duplicate_param: return "duplicate_param";
	}
	return "?";
}

export template <FixedString s>
struct Pattern
{
public:
	static constexpr std::string_view raw = s.view();

	// 两个数组的维度都是**紧**的（不再按 raw 长度过分配）：
	//   segment_count —— 段数。扫描每轮至多 push 一个段、消耗一个 '/' ⇒ 段数 ≤ '/' 的个数；合法 pattern 恰好相等。
	//   name_count    —— 命名捕获的上界。每个命名捕获都在一个 `{...}` 里，而每组恰好一个 '{'（`{$}` 会多算一个，无妨）。
	static constexpr std::size_t segment_count = count_of<s, '/'>();
	static constexpr std::size_t name_count = count_of<s, '{'>();

	// 仅给 `k_node_bound` 用，理由见那边（不能收紧，所以单独一个常量）。
	static constexpr std::size_t node_budget = raw.size() + 1;

	std::array<Segment, segment_count> segs{};
	std::size_t count{0};
	std::array<std::string_view, name_count> names{};
	std::size_t n_names{0};
	ParseError err{ParseError::none};

public:
	constexpr Pattern() noexcept { parse_into(); }

	[[nodiscard]] constexpr bool ok() const noexcept { return err == ParseError::none; }
	[[nodiscard]] constexpr SegKind kind(std::size_t k) const noexcept { return segs[k].kind; }
	[[nodiscard]] constexpr std::string_view text(std::size_t k) const noexcept { return segs[k].text; }
	[[nodiscard]] constexpr PatternView view() const noexcept { return {segs.data(), count, raw}; }

	// 命名捕获，按出现顺序；`{$}` 与尾斜杠产生的匿名全捕获不计入。
	[[nodiscard]] constexpr std::span<const std::string_view> params() const noexcept
	{
		return {names.data(), n_names};
	}

	constexpr void push(SegKind k, std::string_view t, std::size_t pos, std::size_t len) noexcept
	{
		if (count < segment_count)
			segs[count++] = Segment{
				k, t, static_cast<std::uint32_t>(pos), static_cast<std::uint32_t>(len)
			};

		if (k != SegKind::literal && !t.empty() && n_names < name_count)
			names[n_names++] = t;
	}

	[[nodiscard]] constexpr bool has_name(std::string_view n) const noexcept
	{
		for (std::size_t k = 0; k < count; ++k)
			if (segs[k].kind != SegKind::literal && segs[k].text == n)
				return true;
		return false;
	}

	[[nodiscard]] static constexpr bool valid_name(std::string_view n) noexcept
	{
		return !n.empty() && n.find_first_of("{}/") == std::string_view::npos;
	}

	constexpr void parse_into() noexcept
	{
		if (!raw.starts_with('/'))
		{
			err = ParseError::must_start_with_slash;
			return;
		}

		std::size_t i = 1;
		for (;;)
		{
			const std::size_t stop = raw.find('/', i);
			const bool last = (stop == std::string_view::npos);
			const std::size_t j = last ? raw.size() : stop;

			const std::string_view seg = raw.substr(i, j - i);

			if (seg.empty())
			{
				// 末尾空段 = 尾斜杠 → 匿名全捕获；pattern 就是 "/" 时等价于"匹配一切"。
				if (!last)
				{
					err = ParseError::empty_segment;
					return;
				}
				push(SegKind::catch_all, {}, i - 1, 0);
			}
			else if (seg.starts_with('{'))
			{
				if (seg.size() < 2 || !seg.ends_with('}'))
				{
					err = ParseError::orphan_brace;
					return;
				}
				const std::string_view inner = seg.substr(1, seg.size() - 2);
				if (inner.empty())
				{
					err = ParseError::empty_param_name;
					return;
				}

				if (inner == "$")
				{
					push(SegKind::end_mark, {}, i - 1, j - i);
				}
				else if (inner.size() > 3 && inner.ends_with("..."))
				{
					const std::string_view name = inner.substr(0, inner.size() - 3);
					if (!valid_name(name)) { err = ParseError::bad_param_name; return; }
					if (!last) { err = ParseError::catch_all_not_last; return; }
					if (has_name(name)) { err = ParseError::duplicate_param; return; }
					push(SegKind::catch_all, name, i - 1, j - i);
				}
				else
				{
					if (!valid_name(inner)) { err = ParseError::bad_param_name; return; }
					if (has_name(inner)) { err = ParseError::duplicate_param; return; }
					push(SegKind::param, inner, i - 1, j - i);
				}
			}
			else
			{
				if (seg.find_first_of("{}") != std::string_view::npos)
				{
					err = ParseError::orphan_brace;
					return;
				}
				push(SegKind::literal, seg, i - 1, j - i);
			}

			if (last)
				break;
			i = j + 1;
		}
	}
};

inline constexpr std::uint32_t k_no_route = 0xFFFF'FFFFu;
// end_mark 只吃空段，literal/param 只吃非空段 → 这两类对不上同一个段。
[[nodiscard]] constexpr bool segments_disjoint(SegKind a, std::string_view at, SegKind b, std::string_view bt) noexcept
{
	if(a == b) return a == SegKind::literal && at != bt;
	if(a == SegKind::end_mark) return b == SegKind::literal || b == SegKind::param;
	if(b == SegKind::end_mark) return a == SegKind::literal || a == SegKind::param;
	return false;
}

// 两条路由是否会为同一路径争抢。规则：**分歧点上必须有字面量**——有字面量则它 rank 最高、
// 一眼看得出谁赢；两个通配符抢同一位置则没有字面量可定优先级 → 报错。公共前缀完全同形且
// 等长 → 落在同一节点，后注册的覆盖前一个。
// ⚠️ 与 Go 1.22 有意相反（见 `docs/http-server-plan.md` §11.2 B），别"顺手对齐"回去。
[[nodiscard]] constexpr bool routes_conflict(PatternView p, PatternView q) noexcept
{
	const std::size_t common = p.size < q.size ? p.size : q.size;

	for(std::size_t i = 0; i < common; ++i)
	{
		const Segment& x = p.data[i];
		const Segment& y = q.data[i];

		if(segments_disjoint(x.kind, x.text, y.kind, y.text)) return false;
		if(x.kind == y.kind) continue;
		if(x.kind == SegKind::literal || y.kind == SegKind::literal) return false;

		return true;
	}

	return p.size == q.size;
}

struct RouteConflict
{
public:
	std::uint32_t a{k_no_route};
	std::uint32_t b{k_no_route};

public:
	[[nodiscard]] constexpr bool ok() const noexcept { return a == k_no_route; }
};

// ═══ 字符级压缩 radix —— 运行期路径匹配 ═══
// label 都是某个 pattern 的 `raw` 上的切片（静态存储，零拷贝）；只有通配符会让树分叉。
// 建树是 `constexpr` 的，所以同一份代码既能在编译期产出静态树，也能在运行期重建。

inline constexpr std::uint32_t k_none = 0xFFFF'FFFFu;

enum class RadixKind : std::uint8_t
{
	chars,	   // 字符片段（含 '/' 分隔）
	param,	   // {name}
	catch_all, // {name...}，或尾斜杠产生的匿名全捕获
	end_mark,  // {$}
};

struct RadixEdge
{
	RadixKind kind{RadixKind::chars};
	std::string_view label{};  // chars=字符片段；param/catch_all=参数名
	std::uint32_t child{k_none};
	std::uint32_t next{k_none}; // 同一节点内的下一条出边；链按 rank 升序
};

struct RadixNode
{
	std::uint32_t first_edge{k_none}; // 出边链头
	std::uint32_t route{k_no_route};  // 走到本节点**且路径恰好走完**才命中该槽位
};

// 节点数进类型。不变量：每个非根节点恰好一条入边 ⇒ `edges == nodes - 1`。
template <std::size_t node_capacity>
struct RadixTree
{
	static constexpr std::size_t edge_capacity = (node_capacity == 0) ? 0 : node_capacity - 1;

	std::array<RadixNode, node_capacity> nodes{};
	std::array<RadixEdge, edge_capacity> edges{};
	std::uint32_t node_count{0};
	std::uint32_t edge_count{0};
};

struct Capture
{
	std::string_view name;
	std::string_view value;
};

// 同一节点内出边的优先级：字符 → 参数 → 全捕获 → 精确尾。
[[nodiscard]] constexpr int edge_rank(RadixKind k) noexcept
{
	switch(k)
	{
		case RadixKind::chars: return 0;
		case RadixKind::param: return 1;
		case RadixKind::catch_all: return 2;
		case RadixKind::end_mark: return 3;
	}
	return 4;
}

[[nodiscard]] constexpr RadixKind to_radix(SegKind k) noexcept
{
	switch(k)
	{
		case SegKind::param: return RadixKind::param;
		case SegKind::catch_all: return RadixKind::catch_all;
		case SegKind::end_mark: return RadixKind::end_mark;
		default: return RadixKind::chars;
	}
}

// 建树用的“片段”：连续的字面量段折成一个字符片段，通配符各占一个片段。
struct Piece
{
	RadixKind kind{RadixKind::chars};
	std::string_view label{};
};

struct PieceCursor
{
public:
	PatternView pattern{};
	std::size_t seg{0};
	std::size_t consumed{0}; // 当前字符片段已被吃掉多少字符

public:
	[[nodiscard]] constexpr bool done() const noexcept { return seg >= pattern.size; }

	[[nodiscard]] constexpr Piece base() const noexcept
	{
		const Segment& s = pattern.data[seg];
		if(s.kind != SegKind::literal)
			return {to_radix(s.kind), s.text};

		std::size_t end = seg;
		while(end + 1 < pattern.size && pattern.data[end + 1].kind == SegKind::literal)
			++end;

		const std::size_t from = pattern.data[seg].pos;
		const std::size_t to = pattern.data[end].pos + 1 + pattern.data[end].raw_len;
		return {RadixKind::chars, pattern.raw.substr(from, to - from)};
	}

	[[nodiscard]] constexpr Piece current() const noexcept
	{
		Piece p = base();
		if(p.kind == RadixKind::chars) p.label = p.label.substr(consumed);
		return p;
	}

	constexpr void advance() noexcept
	{
		consumed = 0;
		if(done()) return;

		if(pattern.data[seg].kind != SegKind::literal)
		{
			++seg;
			return;
		}

		while(seg < pattern.size && pattern.data[seg].kind == SegKind::literal)
			++seg;
	}

	// 只对字符片段有效；吃完了就自动进到下一个片段。
	constexpr void consume(std::size_t n) noexcept
	{
		if(done()) return;
		if(base().kind != RadixKind::chars)
		{
			advance();
			return;
		}

		consumed += n;
		if(consumed >= base().label.size()) advance();
	}
};

// ── 建树：固定容量，不再用 std::vector ──────────────────────────────
// 中间状态（下一个空闲下标）显式带着走，而不是藏在树里改来改去。
template <std::size_t node_capacity>
struct Builder
{
	RadixTree<node_capacity> tree{};
	std::uint32_t nodes{1}; // 根占了 0
	std::uint32_t edges{0};
};

template <std::size_t node_capacity>
[[nodiscard]] constexpr std::uint32_t add_node(Builder<node_capacity>& b) noexcept { return b.nodes++; }

// 按 rank 升序插进出边链；同 rank 插在已有的之后（→ 保持注册顺序的稳定性）。
template <std::size_t node_capacity>
constexpr void add_edge(Builder<node_capacity>& b, std::uint32_t n, RadixEdge e) noexcept
{
	const auto idx = b.edges++;

	std::uint32_t* link = &b.tree.nodes[n].first_edge;
	while(*link != k_none && edge_rank(b.tree.edges[*link].kind) <= edge_rank(e.kind)) link = &b.tree.edges[*link].next;

	e.next = *link;
	b.tree.edges[idx] = e;
	*link = idx;
}

template <std::size_t node_capacity>
[[nodiscard]] constexpr std::uint32_t find_edge(const Builder<node_capacity>& b, std::uint32_t n, Piece p) noexcept
{
	for(std::uint32_t i = b.tree.nodes[n].first_edge; i != k_none; i = b.tree.edges[i].next)
	{
		const RadixEdge& e = b.tree.edges[i];

		if(p.kind != RadixKind::chars)
		{
			if(e.kind == p.kind) return i;
			continue;
		}

		if(e.kind == RadixKind::chars && !e.label.empty() && !p.label.empty() && e.label.front() == p.label.front())
			return i;
	}

	return k_none;
}

// 把游标剩下的片段挂成一条链，末端记 slot。
template <std::size_t node_capacity>
constexpr void append_chain(Builder<node_capacity>& b, std::uint32_t n, PieceCursor c, std::uint32_t slot) noexcept
{
	while(!c.done())
	{
		const Piece p = c.current();
		const auto child = add_node(b);
		add_edge(b, n, RadixEdge{p.kind, p.label, child, k_none});
		n = child;
		c.advance();
	}

	b.tree.nodes[n].route = slot;
}

template <std::size_t node_capacity>
constexpr void insert_pattern(Builder<node_capacity>& b, PatternView pattern, std::uint32_t slot) noexcept
{
	PieceCursor c{pattern, 0, 0};
	std::uint32_t n = 0; // 根

	while(!c.done())
	{
		const Piece p = c.current();
		const std::uint32_t ei = find_edge(b, n, p);

		if(ei == k_none)
		{
			append_chain(b, n, c, slot);
			return;
		}

		// 固定数组不会重分配，所以这个引用在整个循环里都有效。
		RadixEdge& e = b.tree.edges[ei];

		if(p.kind != RadixKind::chars)
		{
			n = e.child;
			c.advance();
			continue;
		}

		const std::string_view label = e.label;
		std::size_t common = 0;
		while(common < label.size() && common < p.label.size() && label[common] == p.label[common]) ++common;

		if(common == label.size())
		{
			n = e.child;
			c.consume(common);
			continue;
		}

		// 分叉：中间插一个节点；两半仍是 raw 上的切片。
		const auto middle = add_node(b);
		add_edge(b, middle, RadixEdge{RadixKind::chars, label.substr(common), e.child, k_none});
		e.label = label.substr(0, common);
		e.child = middle;

		n = middle;
		c.consume(common);
	}

	b.tree.nodes[n].route = slot;
}

// 折叠：把整包 pattern 按顺序插进空树（下标就在类型里，不用可变计数器）。
template <std::size_t node_capacity, std::uint32_t slot, FixedString first, FixedString... rest>
constexpr void insert_all(Builder<node_capacity>& b) noexcept
{
	insert_pattern(b, Pattern<first>{}.view(), slot);
	if constexpr(sizeof...(rest) > 0) insert_all<node_capacity, slot + 1, rest...>(b);
}

template <std::size_t node_capacity, FixedString... patterns>
[[nodiscard]] constexpr RadixTree<node_capacity> build_into() noexcept
{
	Builder<node_capacity> b{};
	insert_all<node_capacity, 0, patterns...>(b);

	// 收尾：把游标写回树。先算那一步就是靠这两个字段读回规模的。
	b.tree.node_count = b.nodes;
	b.tree.edge_count = b.edges;
	return b.tree;
}

// 每 pattern 的节点上界按 `raw.size() + 1` 估（宽松）⇒ 节点数上界 = Σ(node_budget) + 1。
// 为什么这里**不能**收紧成“段数”：end_mark 与匿名 catch_all 的边 label 是**空**的，
// “#edges ≤ Σ 字符数”那条论证不适用。保持原值 ⇒ 建树行为一字不变。
template <FixedString... patterns>
inline constexpr std::size_t k_node_bound = (Pattern<patterns>::node_budget + ... + 0) + 1;

// 「先算」：用宽松容量跑一遍，只读回实际用了多少节点。
template <FixedString... patterns>
consteval std::size_t node_count() noexcept
{
	return build_into<k_node_bound<patterns...>, patterns...>().node_count;
}

// 「后建」：按**精确**规模实例化。于是整棵树可以是 constexpr 变量（不再有 std::vector）。
template <FixedString... patterns>
[[nodiscard]] constexpr RadixTree<node_count<patterns...>()> build() noexcept
{
	return build_into<node_count<patterns...>(), patterns...>();
}

// 站在节点 n、路径已走到 pos。
template <std::size_t node_capacity>
[[nodiscard]] constexpr bool match_at(
	const RadixTree<node_capacity>& t, std::uint32_t n, std::string_view path, std::size_t pos,
	std::vector<Capture>& out, std::uint32_t& slot
) noexcept
{
	if(t.nodes[n].route != k_no_route && pos == path.size())
	{
		slot = t.nodes[n].route;
		return true;
	}

	for(std::uint32_t i = t.nodes[n].first_edge; i != k_none; i = t.edges[i].next)
	{
		const RadixEdge& e = t.edges[i];
		const auto checkpoint = out.size();
		bool taken = true;
		std::size_t next = pos;

		switch(e.kind)
		{
			case RadixKind::chars:
				if(!path.substr(pos).starts_with(e.label)) taken = false;
				else next = pos + e.label.size();
				break;

			case RadixKind::param:
			{
				if(pos >= path.size() || path[pos] != '/') { taken = false; break; }
				const std::size_t slash = path.find('/', pos + 1);
				const std::size_t end = (slash == std::string_view::npos) ? path.size() : slash;
				if(end == pos + 1) { taken = false; break; } // 空段不算捕获
				out.push_back({e.label, path.substr(pos + 1, end - pos - 1)});
				next = end;
				break;
			}

			case RadixKind::catch_all:
				if(pos >= path.size() || path[pos] != '/') { taken = false; break; }
				if(!e.label.empty()) out.push_back({e.label, path.substr(pos + 1)});
				next = path.size();
				break;

			case RadixKind::end_mark:
				if(pos >= path.size() || path[pos] != '/' || pos + 1 != path.size())
				{
					taken = false;
					break;
				}
				next = path.size();
				break;
		}

		if(taken && match_at(t, e.child, path, next, out, slot)) return true;
		out.resize(checkpoint);
	}

	return false;
}

// 命中返回槽位（否则 k_no_route）；out 按边出现顺序装填捕获。
template <std::size_t node_capacity>
[[nodiscard]] constexpr std::uint32_t match(
	const RadixTree<node_capacity>& t, std::string_view path, std::vector<Capture>& out
) noexcept
{
	std::uint32_t slot = k_no_route;
	out.clear();
	if(match_at(t, 0, path, 0, out, slot)) return slot;
	out.clear();
	return k_no_route;
}

// ═══ 组级冲突检查 ═══
// `methods` 与 `patterns` 按下标对齐；返回第一对冲突的**下标**。
[[nodiscard]] constexpr RouteConflict find_table_conflict(
	std::span<const std::string_view> methods, std::span<const PatternView> patterns
) noexcept
{
	for(std::size_t i = 0; i < patterns.size(); ++i)
		for(std::size_t j = i + 1; j < patterns.size(); ++j)
		{
			const bool overlapping = methods[i] == methods[j];
			if(overlapping && routes_conflict(patterns[i], patterns[j]))
				return {static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j)};
		}

	return {};
}

// ═══ 路由声明 DSL：动词 + 路径住在**第一个形参** ═══
// 用法见 `docs/routing.md`；这里只有一条契约：第一个形参既是路由声明、也是本次的请求。

export enum class Verb : std::uint8_t
{
	get,
	post,
	put,
	del,
	head,
	options,
	patch,
};

[[nodiscard]] constexpr std::string_view to_string(Verb verb) noexcept
{
	switch(verb)
	{
		case Verb::get: return "GET";
		case Verb::post: return "POST";
		case Verb::put: return "PUT";
		case Verb::del: return "DELETE";
		case Verb::head: return "HEAD";
		case Verb::options: return "OPTIONS";
		case Verb::patch: return "PATCH";
	}
	return "";
}

// 一条路由的请求：既是**路由声明**（verb + pattern 在编译期），也带**本次请求的数据**。
export template <Verb verb, FixedString pattern>
class Request
{
public:
	static constexpr Verb method = verb;
	static constexpr auto literal = pattern; // 可当模板实参（按成员逐项结构等价）
	static constexpr std::string_view route_pattern = pattern.view();

	// 类别的**稳定标识**：`method + pattern` 的 FNV-1a —— 观测归因与“指纹 → 参数”表的键（§4.1.4）。
	// ⚠️ 必须**稳定**：不吃编译器的类型 hash（那会随编译选项 / 版本变）。
	[[nodiscard]] static constexpr std::uint64_t fingerprint() noexcept
	{
		std::uint64_t h = 1469598103934665603ull;
		for(const char c : to_string(verb)) h = (h ^ static_cast<std::uint64_t>(c)) * 1099511628211ull;
		h = (h ^ static_cast<std::uint64_t>(':')) * 1099511628211ull;
		for(const char c : pattern.view()) h = (h ^ static_cast<std::uint64_t>(c)) * 1099511628211ull;
		return h;
	}

	// 命名捕获在 pattern 里的下标；没这个名字 → `params().size()`（越界，正是 `param()` 用来拦错的判据）。
	// ⚠️ 不能用 static 局部变量存 `Pattern`（`params()` 的 span 指向它，而 constexpr 函数不许有静态变量）。
	[[nodiscard]] static constexpr std::size_t slot_of(std::string_view name) noexcept
	{
		const Pattern<pattern> parsed{};
		const std::span<const std::string_view> names = parsed.params();
		for(std::size_t i = 0; i < names.size(); ++i)
			if(names[i] == name) return i;
		return names.size();
	}

	using executor_type = asio::any_io_executor; // asio 的流概念要求这个成员类型

public:
	// 框架专用（`make_arg` 调它）；用户不要直接构造。
	// `constexpr` 是为了让编译期自检能直接构造一个请求对象（见 `server/route.checks.cpp`）；
	// 那时没有连接 ⇒ `in` 留空 —— 那种场合只有 `param<>()` 这类“读报文数据”的用法是安全的。
	constexpr Request(const http::Request& raw, std::span<const Capture> captures, http::BodyReader* in = nullptr) noexcept :
		raw_(raw),
		captures_(captures),
		in_(in)
	{}

public:
	// 报文数据（框架解析出来的 start line + headers）。
	[[nodiscard]] const http::Request& raw() const noexcept { return raw_; }

public:
	// 读本请求的体：视图把“报文数据”与“读通道”合到一处，handler 用起来还是一件东西。
	// ⚠️ 体是**字节**：要文本语义得自己 `http::as_text(...)`。
	[[nodiscard]] asio::awaitable<std::vector<std::byte>> body() const
	{
		if(in_ == nullptr) co_return std::vector<std::byte>{};
		co_return co_await http::read_body(*in_, raw_.has_content_length ? raw_.content_length : 0);
	}
	asio::awaitable<std::size_t> read_some(std::span<std::byte> out) const
	{
		if(in_ == nullptr) co_return 0;
		co_return co_await in_->read_some(out);
	}

	// 读通道 ⇒ 拿着 handler 里的 `req` 就能用 asio 的组合操作（`asio::async_read` …）。
	[[nodiscard]] executor_type get_executor() const noexcept
	{
		return in_ != nullptr ? in_->get_executor() : asio::any_io_executor{};
	}
	template <typename MutableBufferSequence, typename CompletionToken>
	auto async_read_some(const MutableBufferSequence& buffers, CompletionToken&& token) const
	{
		return in_->async_read_some(buffers, std::forward<CompletionToken>(token));
	}

public:
	// 按名字取命名捕获 —— 名字在**编译期**对着 pattern 校对（写错就编不过）。
	// 整数走 `from_chars`（与旧的位置注入同语义：转换失败静默给 0）；string_view 直通。
	template <class T, FixedString name>
	[[nodiscard]] constexpr T param() const noexcept
	{
		constexpr std::size_t slot = slot_of(name.view());
		static_assert(slot < Pattern<pattern>{}.params().size(), "路由声明：路径里没有这个名字的命名捕获");

		const std::string_view text = slot < captures_.size() ? captures_[slot].value : std::string_view{};

		if constexpr(std::is_same_v<T, std::string_view>)
		{
			return text;
		}
		else if constexpr(std::is_integral_v<T>)
		{
			T value{};
			std::from_chars(text.data(), text.data() + text.size(), value);
			return value;
		}
		else
		{
			static_assert(!sizeof(T), "路由注入：param<> 目前只支持整数与 string_view");
		}
	}

private:
	// 数据全私有（`class` 的约定）；要暴露就加访问方法（例：上面的 `raw()`）。
	const http::Request& raw_; // 框架解析出来的 start line + headers
	std::span<const Capture> captures_{}; // 本次匹配出的命名捕获，生存期覆盖 handler 执行期间
	http::BodyReader* in_{nullptr}; // 读通道（连接层实现）：框架在派发时给（见上面的构造函数）
};

export template <FixedString pattern>
using Get = Request<Verb::get, pattern>;
export template <FixedString pattern>
using Post = Request<Verb::post, pattern>;
export template <FixedString pattern>
using Put = Request<Verb::put, pattern>;
export template <FixedString pattern>
using Delete = Request<Verb::del, pattern>;
export template <FixedString pattern>
using Head = Request<Verb::head, pattern>;
export template <FixedString pattern>
using Options = Request<Verb::options, pattern>;
export template <FixedString pattern>
using Patch = Request<Verb::patch, pattern>;

// ═══ 响应侧：给 handler 用的门面 ═══
// ⚠️ 状态**不在**响应对象里 —— 它们在 `http::Context`（每请求一份，可复用）；
//    这里只是它的一个**薄视图** + 写通道的入口（什么时候写出去由 handler / 类型自己定）。
//    三种类型 = 三种 framing 策略；链式 setter 返回具体类型（CRTP），所以能混着链。

// 响应侧的门面：状态住在 `http::Context` 里，这里只做“读写状态 + 发到通道”。
// ⚠️ 链式 setter 用 **deducing this**（C++23 显式对象参数）：`this Self&& self` 推导出具体类型，
//    返回 `self` 就是“返回具体类型” ⇒ 于是**不需要 CRTP**，三个响应类型只需单继承。
export class ResponseCore
{
public:
	ResponseCore(const ResponseCore&) = delete;
	ResponseCore& operator=(const ResponseCore&) = delete;
	ResponseCore(ResponseCore&&) noexcept = default;
	ResponseCore& operator=(ResponseCore&&) noexcept = default;

public:
	[[nodiscard]] int status() const { return state().status; }
	[[nodiscard]] std::string_view content_type() const { return state().content_type; }
	[[nodiscard]] std::span<const http::Header> headers() const { return state().headers; }

public:
	template <typename Self>
	auto&& status(this Self&& self, int code)
	{
		self.require_config("status");
		self.state().status = code;
		return std::forward<Self>(self);
	}

	template <typename Self>
	auto&& content_type(this Self&& self, std::string_view value)
	{
		self.require_config("content_type");
		self.state().content_type = value;
		return std::forward<Self>(self);
	}

	template <typename Self>
	auto&& header(this Self&& self, std::string_view name, std::string_view value)
	{
		self.require_config("header");
		self.state().headers.push_back({std::string{name}, std::string{value}});
		return std::forward<Self>(self);
	}

	// 预告尾部要补哪些字段（只对 chunked 有意义；SHOULD 预告 —— RFC 9110 §6.6.2）。
	template <typename Self>
	auto&& trailer(this Self&& self, std::string_view name)
	{
		self.require_config("trailer");
		self.state().trailer_names.emplace_back(name);
		return std::forward<Self>(self);
	}

public:
	// 框架在调 handler 之前注入：状态的家（`Context`）+ 写通道。
	// ⚠️ 顺带把上一次请求的响应状态清干净（Context 跨请求复用）⇒ **配置必须在 attach 之后**。
	// 用显式对象参数是为了让 `self.apply_defaults()` 落到**具体类型**那份（基类只留个空实现）。
	template <typename Self>
	void attach(this Self&& self, http::Context& context, http::ResponseWriter& writer) noexcept
	{
		self.ctx_ = &context;
		self.out_ = &writer;
		context.response().reset();
		self.apply_defaults();
	}

	// 各响应类型的初始状态（`EmptyResponse` 默认 204、文本类型默认 `text/plain` …）；框架 attach 时叫它。
	void apply_defaults() noexcept {}

protected:
	ResponseCore() = default;

	// 状态就在 `http::Context` 里 —— 响应对象只是它的一个薄门面。
	[[nodiscard]] http::ResponseState& state()
	{
		if(ctx_ == nullptr) throw std::logic_error{"响应状态还没就位：只能由框架 attach(Context&, ResponseWriter&) 之后用"};
		return ctx_->response();
	}

	[[nodiscard]] const http::ResponseState& state() const
	{
		if(ctx_ == nullptr) throw std::logic_error{"响应状态还没就位：只能由框架 attach(Context&, ResponseWriter&) 之后用"};
		return ctx_->response();
	}

	void require_config(std::string_view action) const
	{
		if(state().stage != http::Stage::config) http::throw_out_of_order(action);
	}

	void require_writer() const
	{
		if(out_ == nullptr) throw std::logic_error{"响应写通道未注入（只能由框架在调 handler 之前 attach）"};
	}

	// 把响应头发出去；framing 由派生类型给（成帧是协议知识，所以调 `:http` 的纯函数）。成功 ⇒ head_sent。
	asio::awaitable<std::error_code> send_head(std::optional<std::uint64_t> content_length, bool chunked)
	{
		require_writer();
		auto& s = state();

		std::vector<http::Header> hint; // `Trailer:` 预告只要名字，值在末块之后补
		hint.reserve(s.trailer_names.size());
		for(const std::string& name : s.trailer_names) hint.push_back({name, {}});

		const auto head = http::head_bytes(
			s.status, s.content_type, s.headers, content_length, chunked, out_->keep_alive(), hint
		);
		const auto ec = co_await out_->write(std::as_bytes(std::span{head}));
		if(!ec) s.stage = http::Stage::head_sent;
		co_return ec;
	}

protected:
	// 成员放末尾；派生类型（`StringResponse::flush` 等）要直接用它发头 / 发体 ⇒ protected 而非 private。
	http::Context* ctx_{nullptr};		   // 状态的家（框架 attach 时给）
	http::ResponseWriter* out_{nullptr}; // 写通道（同上）
};

// 有体的响应：`body(块)` 只往内存追加，出网是 flush / finish 的事。
// 文本写法另给 `string_view` 重载；streaming = false 时头一发出就不许再追加。
export template <bool streaming>
class BodyResponse : public ResponseCore
{
public:
	template <typename Self>
	auto&& body(this Self&& self, std::span<const std::byte> data)
	{
		self.require_appendable();
		auto& pending = self.state().pending;
		pending.insert(pending.end(), data.begin(), data.end());
		return std::forward<Self>(self);
	}

	// 文本当体（`res.body("ok\n")` 走这个，零拷贝）。
	template <typename Self>
	auto&& body(this Self&& self, std::string_view text)
	{
		return std::forward<Self>(self).body(std::as_bytes(std::span{text}));
	}

public:
	// 还没发出去的体（定长类型 = 整个体）。
	[[nodiscard]] std::span<const std::byte> body() const { return state().pending; }

protected:
	BodyResponse() = default;

private:
	void require_appendable() const
	{
		if constexpr(streaming)
		{
			if(state().stage == http::Stage::finished) http::throw_out_of_order("body");
		}
		else
		{
			require_config("body");
		}
	}
};

// 有体、长度已知：长度就是 `body()` 攒下的字节数 ⇒ `Content-Length` 定界。
export struct StringResponse : BodyResponse<false>
{
public:
	void apply_defaults() noexcept { state().content_type = "text/plain; charset=utf-8"; }

	// flush：发头 + 发体（HEAD 只发头）。定长体一次性发完 ⇒ flush 即收尾。
	asio::awaitable<std::error_code> flush();
	// 收尾（= flush，幂等）：给框架兜底与推式响应一个统一的形状。
	asio::awaitable<std::error_code> finish() { return flush(); }
};

// 无体响应。⚠️ 刻意**没有** `body(…)` —— 无体响应不接受体（编译期拦）。
// 204 / 304 / 1xx 不得带 `Content-Length`（RFC 9110 §8.6）；其它状态码要 `Content-Length: 0` 才能定界。
export struct EmptyResponse : ResponseCore
{
public:
	void apply_defaults() noexcept { state().status = 204; }

	asio::awaitable<std::error_code> flush();
	asio::awaitable<std::error_code> finish() { return flush(); }
};

// 长度未知 ⇒ 响应侧 chunked：`body(块)` 攒进内存，`flush()` 的时候才作为一个块推出去。
export struct UnknownLengthResponse : BodyResponse<true>
{
public:
	void apply_defaults() noexcept { state().content_type = "text/plain; charset=utf-8"; }

	// flush：头没发就先发头，然后把攒着的字节作为一个块发出去。
	asio::awaitable<std::error_code> flush();
	// 收尾：flush + 末块 + trailer（幂等）。handler 不调的话由框架兜底。
	asio::awaitable<std::error_code> finish(std::span<const http::Header> trailers = {});
};

// 响应类型的判据（`make_arg` 据此区分“请求形参”与“响应形参”）：
// 框架要能给它注入 Context + 写通道、能兜底收尾。
export template <typename T>
concept IsResponse = requires(T& response, http::Context& context, http::ResponseWriter& writer) {
	{ response.attach(context, writer) } -> std::same_as<void>;
	{ response.finish() } -> std::same_as<asio::awaitable<std::error_code>>;
};

// 形参类型分类（`make_arg` 与编译期诊断用）
template <class>
struct IsRouteRequest : std::false_type
{
};

template <Verb verb, FixedString pattern>
struct IsRouteRequest<Request<verb, pattern>> : std::true_type
{
};

// 成员指针 → 动词 / 路径 / 形参表。分解交给 jk 的 `Callable`：它覆盖 cv(4) × ref(3) × noexcept(2) 全组合。
// 动词 + pattern 从**第一个形参**取。
template <class Pmf>
struct RouteOf
{
	using Result = JK::Meta::Return<Pmf>; // 应当是 `asio::awaitable<void>`
	using Params = JK::Meta::Arguments<Pmf>;
	using Route = typename Params::First; // 第一个形参：`Get<…>` / `Post<…>` / …

	static_assert(IsRouteRequest<Route>::value,
				  "路由声明：第一个形参必须是 `Get<\"…\">` / `Post<\"…\">` 这样的请求类型（声明写在形参位置）");
	static_assert(std::is_same_v<Result, asio::awaitable<void>>,
				  "路由声明：handler 必须返回 `asio::awaitable<void>`（响应按引用填，见 http-server-plan.md §4.1.1）");

	static constexpr Verb verb = Route::method;
	static constexpr auto pattern = Route::literal;
	static constexpr std::string_view pattern_view = Route::route_pattern;
};

// 故意不定义：实例化它就是为了让编译错误里出现这两条冲突的 pattern。
template <FixedString a, FixedString b>
struct ShadowedRoute;

// 第 i 个路由的 pattern（给上面那个诊断用）。
template <std::size_t i, class... Pmf>
inline constexpr auto nth_pattern = std::tuple_element_t<i, std::tuple<RouteOf<Pmf>...>>::pattern;

// 单条路由的 pattern 实例。必须是**静态**的：`Pattern::view()` 返回的指针指进它自己，
// 用临时对象的话那条指针就不是常量表达式（而 find_table_conflict 收的正好是这种指针）。
template <class Pmf>
struct SpecOf
{
	static constexpr Pattern<RouteOf<Pmf>::pattern> value{};
};

// 把 pattern 挑成一份名单（每 method 一棵树的基础）。
template <FixedString... Ps>
struct PatternList
{
};

template <FixedString s, class List>
struct PrependPattern;

template <FixedString s, FixedString... Ps>
struct PrependPattern<s, PatternList<Ps...>>
{
	using type = PatternList<s, Ps...>;
};

template <Verb verb, class... Pmf>
struct SelectPatterns;

template <Verb verb>
struct SelectPatterns<verb>
{
	using type = PatternList<>;
};

template <Verb verb, class First, class... Rest>
struct SelectPatterns<verb, First, Rest...>
{
	using kept = typename PrependPattern<RouteOf<First>::pattern, typename SelectPatterns<verb, Rest...>::type>::type;
	using type = std::conditional_t<RouteOf<First>::verb == verb, kept,
									typename SelectPatterns<verb, Rest...>::type>;
};

template <class>
struct IsEmptyPatternList : std::false_type
{
};

template <>
struct IsEmptyPatternList<PatternList<>> : std::true_type
{
};

template <FixedString... Ps>
[[nodiscard]] constexpr auto build_of(PatternList<Ps...>) noexcept
{
	return build<Ps...>();
}

// 编译期校验一条路由声明（`route_on` 里调用即可触发）。
// ⚠️ 命名捕获的**读取**不再能在这里校验 —— 改由 handler 里的 `req.param<T, "名字">()` 就地校对
// （更精确：错在写的那一行；代价是“pattern 里的名字没人读”不再算错）。
template <class Pmf>
constexpr void validate_route() noexcept
{
	static_assert(Pattern<RouteOf<Pmf>::pattern>{}.ok(), "路由声明：路径 pattern 语法不合法（错误码见 Pattern::err / to_string）");
}

// 一组的（动词, pattern）清单与冲突判定结果。⚠️ 必须是**静态**成员：`find_table_conflict`
// 收的是 span，而局部 constexpr 数组的地址不是常量表达式。
template <class... Pmf>
struct GroupClash
{
	static constexpr std::size_t count = sizeof...(Pmf);
	static constexpr std::array<std::string_view, count> methods{to_string(RouteOf<Pmf>::verb)...};
	static constexpr std::array<PatternView, count> patterns{SpecOf<Pmf>::value.view()...};
	static constexpr RouteConflict value = find_table_conflict(methods, patterns);
};

// 编译期校验一组路由：同一 method 下不得有两条会为同一路径争抢（规则见 docs/http-server-plan.md §11.2 B）。
// 跨组冲突不存在 —— `Builder::route_on` 只允许调一次（返回的 builder 已经没有 route_on 了）。
template <class... Pmf>
constexpr void validate_group() noexcept
{
	static_assert(sizeof...(Pmf) > 0, "route_on 至少要注册一条路由");

	constexpr RouteConflict clash = GroupClash<Pmf...>::value;

	if constexpr(!clash.ok())
	{
		// ShadowedRoute 故意没有定义 → 编译错误里会直接打出这两条冲突的 pattern
		[[maybe_unused]] constexpr std::size_t diagnostic =
			sizeof(ShadowedRoute<nth_pattern<clash.a, Pmf...>, nth_pattern<clash.b, Pmf...>>);
	}
}

// 造一个注入实参，按形参类型分两类：
//   `Get<…>` 这类请求类型 → 包一份请求（报文数据 + 读通道 + 本次捕获；路径参数由 handler 用 `req.param<…>()` 自取）
//   响应类型（`IsResponse`）→ 框架持有的响应对象，handler 往它里面填 / 推
template <std::size_t index, class Pmf, class Response>
[[nodiscard]] decltype(auto) make_arg(
	http::Context& ctx, http::BodyReader& in, Response& response, std::span<const Capture> captures
)
{
	using Params = typename RouteOf<Pmf>::Params;
	using P = typename Params::template At<index>;

	if constexpr(IsRouteRequest<P>::value)
	{
		return P{ctx.request(), captures, &in};
	}
	else if constexpr(IsResponse<std::remove_cvref_t<P>>)
	{
		return response;
	}
	else
	{
		static_assert(!sizeof(P), "路由注入：形参只能是 Get<…>（第 0 个）/ 响应类型（StringResponse& / EmptyResponse& / UnknownLengthResponse&）");
	}
}

// 形参表里“那个响应形参”的下标；没有 → `Params::size()`（越界值，用来拦错）。
template <class Params, std::size_t index = 0>
[[nodiscard]] consteval std::size_t response_index() noexcept
{
	if constexpr(index == Params::size())
	{
		return index;
	}
	else if constexpr(IsResponse<std::remove_cvref_t<typename Params::template At<index>>>)
	{
		return index;
	}
	else
	{
		return response_index<Params, index + 1>();
	}
}

// 一条路由的调用器：**不擦除**（具体到 `C*` + 成员指针），捕获由匹配那一步传进来。
// 形参表里的响应类型在这里被静态确定 ⇒ 响应对象直接构造在协程帧上。
template <class C, class F, std::size_t... indices>
[[nodiscard]] auto make_slot(C* self, F pmf, std::index_sequence<indices...>)
{
	using Params = typename RouteOf<F>::Params;
	static constexpr std::size_t response_slot = response_index<Params>();
	static_assert(
		response_slot < Params::size(),
		"路由 handler 必须有一个响应形参：StringResponse& / EmptyResponse& / UnknownLengthResponse&"
	);

	return [self, pmf](
			   http::Context& ctx, http::BodyReader& in, http::ResponseWriter& writer,
			   std::span<const Capture> captures
		   ) -> asio::awaitable<void> {
		using Response = std::remove_cvref_t<typename Params::template At<response_slot>>;
		Response response;
		response.attach(ctx, writer); // 注入状态的家 + 写通道；之后什么时候写由 handler / 响应类型定
#if PYLAI_ENABLE_OBSERVATION
		// 观测挂点：告诉这次请求命中了哪条路由（类别 = 指纹，见 plan §4.1.4）。
		ctx.observation().route_fingerprint = Request<RouteOf<F>::verb, RouteOf<F>::pattern>::fingerprint();
#endif
		co_await (self->*pmf)(make_arg<indices, F>(ctx, in, response, captures)...);
		co_await response.finish(); // 兜底：handler 没显式收尾时由框架收（幂等）
	};
}

// 挑出属于 `verb` 的槽位，按注册顺序拼成一个 tuple（顺带就是"只留该动词"的过滤）。
// 返回类型随谓词逐项变化 ⇒ 用 `if constexpr` 分两支，`tuple_cat` 把空 tuple 吃掉。
template <Verb verb, std::size_t i, class C, class... Pmf>
[[nodiscard]] auto select_slot(C* self, const std::tuple<Pmf...>& table)
{
	using F = std::tuple_element_t<i, std::tuple<Pmf...>>;
	if constexpr(RouteOf<F>::verb == verb)
	{
		return std::tuple{make_slot(
			self, std::get<i>(table), std::make_index_sequence<RouteOf<F>::Params::size()>{}
		)};
	}
	else
	{
		return std::tuple{};
	}
}

template <Verb verb, class C, class... Pmf, std::size_t... i>
[[nodiscard]] auto select_slots(C* self, const std::tuple<Pmf...>& table, std::index_sequence<i...>)
{
	return std::tuple_cat(select_slot<verb, i>(self, table)...);
}

// 运行期槽位号 → 编译期下标：顺序比到命中为止。槽位数是每条路由的形参表长度，
// 个位数 ⇒ 几层 `if` 比一次 `std::function` 虚调用还便宜，而槽位本身**一个都没被擦除**。
template <std::size_t i, class Slots>
[[nodiscard]] asio::awaitable<bool> invoke_slot(
	std::size_t slot, Slots& slots, http::Context& ctx, http::BodyReader& in,
	http::ResponseWriter& writer, std::span<const Capture> captures
)
{
	if(slot == i)
	{
		co_await std::get<i>(slots)(ctx, in, writer, captures);
		co_return true;
	}
	if constexpr(i + 1 < std::tuple_size_v<Slots>)
	{
		co_return co_await invoke_slot<i + 1>(slot, slots, ctx, in, writer, captures);
	}
	co_return false;
}

// 一个动词 = **一处**擦除单元：内部一棵该动词的编译期树 + 与之按槽位对齐的槽位表，
// 匹配与调用同一个闭包里（外面只看到"这条路径我认不认"与"认了就调用"两个门面）。
// ⚠️ 动词必须分开建树：树只由 pattern 决定、动词不参与 —— 混在一棵树里的话
// `Get<"/x">` 与 `Post<"/x">` 会落到同一节点、互相覆盖槽位（后注册的吃掉先注册的）。
struct VerbRoutes
{
	std::string_view verb;

	// 纯判定（405 / OPTIONS 的慢路径用）：这条路径在本动词下有没有路由。
	std::function<bool(std::string_view path)> matched;

	// 命中就调用；false = 这条路径在本动词下不存在（由调用方决定回 405 还是 404）。
	// ⚠️ 形参必须非模板（`std::function`）⇒ 这里给的是“零件”：Context + 读通道 + 写通道；
	//    `Get<…>` 视图在槽位里组装（`make_arg`）—— 所以视图本身不必穿擦除边界。
	std::function<asio::awaitable<bool>(http::Context&, http::BodyReader&, http::ResponseWriter&)> invoke;
};

template <Verb verb, class C, class... Pmf>
void add_verb_routes(std::vector<VerbRoutes>& routes, C* self, Pmf... pmf)
{
	using Patterns = typename SelectPatterns<verb, Pmf...>::type;
	if constexpr(!IsEmptyPatternList<Patterns>::value)
	{
		static constexpr auto tree = build_of(Patterns{});
		auto slots = select_slots<verb>(self, std::tuple{pmf...}, std::index_sequence_for<Pmf...>{});

		routes.push_back(VerbRoutes{
			to_string(verb),
			[](std::string_view path) {
				std::vector<Capture> scratch;
				return match(tree, path, scratch) != k_no_route;
			},
			[slots = std::move(slots)](
				http::Context& ctx, http::BodyReader& in, http::ResponseWriter& writer
			) -> asio::awaitable<bool> {
				std::vector<Capture> captures;
				const std::uint32_t slot = match(tree, ctx.request().path, captures);
				if(slot == k_no_route) co_return false;
				co_return co_await invoke_slot<0>(slot, slots, ctx, in, writer, captures);
			}
		});
	}
}

// 一次 `route_on` 调用装出来的整张路由表：每个出现过的动词一条。
template <class C, class... Pmf>
[[nodiscard]] std::vector<VerbRoutes> make_routes(C* self, Pmf... pmf)
{
	std::vector<VerbRoutes> routes;
	routes.reserve(7);

	add_verb_routes<Verb::get>(routes, self, pmf...);
	add_verb_routes<Verb::post>(routes, self, pmf...);
	add_verb_routes<Verb::put>(routes, self, pmf...);
	add_verb_routes<Verb::del>(routes, self, pmf...);
	add_verb_routes<Verb::head>(routes, self, pmf...);
	add_verb_routes<Verb::options>(routes, self, pmf...);
	add_verb_routes<Verb::patch>(routes, self, pmf...);
	return routes;
}

} // namespace pylai::route
