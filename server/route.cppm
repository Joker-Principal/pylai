module;

#include <array>
#include <charconv>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module pylai:route;

import asio;
import jk;
import :http;

export namespace pylai::route {

using Handler = std::function<asio::awaitable<http::Response>(const http::Request&)>;

// 每个出现过的动词一张路由表（树 + 槽位表）；定义在文件后半，那里才用得到 Capture。
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

	// 装上 `Builder::route_on` 声明出来的路由表。⚠️ 只该装一次 ——
	// `Builder::route_on` 只允许调一次（它返回的 builder 已经没有 route_on 了）。
	void set_routes(std::vector<VerbRoutes> routes);

	// 命中 → handler；HEAD 没有显式 `Head<>` 时落到同路径的 GET（正文由 serialize() 按 HEAD 抑制）；
	// 路径存在但动词不符 → 405 + `Allow`；OPTIONS 没有显式 `Options<>` 时自动 204 + `Allow`；否则 404。
	asio::awaitable<http::Response> dispatch(const http::Request& request) const;

private:
	// 每个出现过的动词一条。vector 允许不完整类型 —— `VerbRoutes` 定义在文件后半。
	std::vector<VerbRoutes> methods;
};

// ═══ path pattern —— 编译期解析（语法表见 docs/http-server-plan.md §11.2 A）═══

// NTTP 载体；必须是 structural type（成员全 public、无虚函数、无 mutable）。
template <std::size_t n>
struct FixedString
{
	char data[n]{};

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

enum class ParseError : std::uint8_t
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

[[nodiscard]] constexpr const char* to_string(ParseError e) noexcept
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

template <FixedString s>
struct Pattern
{
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
	std::uint32_t a{k_no_route};
	std::uint32_t b{k_no_route};

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
	PatternView pattern{};
	std::size_t seg{0};
	std::size_t consumed{0}; // 当前字符片段已被吃掉多少字符

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

// ═══ 路由声明 DSL：动词 + 路径住在**返回类型**位置 ═══
// 用法（注册入口是 `Server::Builder::route_on`）：
//     class User
//     {
//     public:
//         Get<"/health"> health(const http::Request&);
//         Post<"/submit/{id}"> submit(Param<int> id, const http::Request&);
//     };
// 机制：① `Endpoint` 自身就是协程返回类型（靠下面的 coroutine_traits 特化）；
//       ② 注册时从成员指针取出动词 / 路径 / 形参表，`Param<T>` 按**位置**吃第 N 个命名捕获。

enum class Verb : std::uint8_t
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

// 既是路由声明，也是协程返回对象：`get_return_object()` 产出的 awaitable 由转换构造器接住。
template <Verb verb, FixedString pattern>
class Endpoint
{
public:
	using Inner = asio::awaitable<http::Response, asio::any_io_executor>;

	Endpoint(Inner inner) noexcept : inner(std::move(inner)) {}
	Endpoint(Endpoint&&) noexcept = default;

	static constexpr Verb method = verb;
	static constexpr auto literal = pattern; // 可当模板实参（按成员逐项结构等价）
	static constexpr std::string_view route_pattern = pattern.view();

	// asio 的 frame 用 await_transform 收口、只认自家 awaitable，且先做模板推导（隐式转换无效）→
	// 不能直接 `co_await Endpoint`，必须先取出内部 awaitable。
	[[nodiscard]] Inner release() && noexcept { return std::move(inner); }

private:
	Inner inner;
};

template <FixedString pattern>
using Get = Endpoint<Verb::get, pattern>;
template <FixedString pattern>
using Post = Endpoint<Verb::post, pattern>;
template <FixedString pattern>
using Put = Endpoint<Verb::put, pattern>;
template <FixedString pattern>
using Delete = Endpoint<Verb::del, pattern>;
template <FixedString pattern>
using Head = Endpoint<Verb::head, pattern>;
template <FixedString pattern>
using Options = Endpoint<Verb::options, pattern>;
template <FixedString pattern>
using Patch = Endpoint<Verb::patch, pattern>;

// 注入载体：第 N 个 `Param<T>` 吃第 N 个命名捕获；名字可选，写了就在编译期跟 pattern 校对。
template <class T, FixedString name = "">
struct Param
{
	using Value = T;
	static constexpr std::string_view param_name = name.view();
	T value{};
};

template <class>
struct IsParam : std::false_type
{
};

template <class T, FixedString name>
struct IsParam<Param<T, name>> : std::true_type
{
};

// 成员指针 → 动词 / 路径 / 形参表。分解交给 jk 的 `Callable`：它覆盖 cv(4) × ref(3) × noexcept(2)
// 全组合（手写偏特化只匹配裸形式 `R (C::*)(Ps...)`），而 `Arguments` 是带算法的 `TList`。
template <class Pmf>
struct RouteOf
{
	using Result = JK::Meta::Return<Pmf>;
	using Params = JK::Meta::Arguments<Pmf>;

	static constexpr Verb verb = Result::method;
	static constexpr auto pattern = Result::literal;
	static constexpr std::string_view pattern_view = Result::route_pattern;
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

// 形参表里 `Param<>` 的个数
template <class Params>
struct ParamCount;

template <class... Ps>
struct ParamCount<JK::Meta::TList<Ps...>>
	: std::integral_constant<std::size_t,
							 (std::size_t{0} + ... + (IsParam<Ps>::value ? std::size_t{1} : std::size_t{0}))>
{
};

// 第 index 个形参吃第几个捕获 = 它**前面**有多少个 `Param<>`（`Slice<0, index>` 就是“前面那些形参”）。
template <std::size_t index, class Params>
struct SlotOf : ParamCount<typename Params::template Slice<0, index>>
{
};

// 带名字的 `Param<T, "id">` 与 pattern 第 N 个捕获名做编译期校对
template <std::size_t index, class Pmf>
constexpr void check_param_name() noexcept
{
	using Params = typename RouteOf<Pmf>::Params;
	using P = typename Params::template At<index>;

	if constexpr(IsParam<P>::value)
	{
		constexpr auto slot = SlotOf<index, Params>::value;
		// 直接用临时对象取值，不要把 params() 的 span 存下来（它指向临时对象的成员）
		static_assert(Pattern<RouteOf<Pmf>::pattern>{}.params().size() > slot,
					  "路由声明：Param<> 的序号超出了路径里的命名捕获");

		if constexpr(!P::param_name.empty())
			static_assert(P::param_name == Pattern<RouteOf<Pmf>::pattern>{}.params()[slot],
						  "路由声明：Param 的名字与路径里对应位置的捕获名不一致（位置注入按顺序配对）");
	}
}

template <class Pmf, std::size_t... indices>
constexpr void check_all_param_names(std::index_sequence<indices...>) noexcept
{
	(check_param_name<indices, Pmf>(), ...);
}

// 编译期校验一条路由声明（`route_on` 里调用即可触发）
template <class Pmf>
constexpr void validate_route() noexcept
{
	using Params = typename RouteOf<Pmf>::Params;

	static_assert(Pattern<RouteOf<Pmf>::pattern>{}.ok(), "路由声明：路径 pattern 语法不合法（错误码见 Pattern::err / to_string）");
	static_assert(ParamCount<Params>::value == Pattern<RouteOf<Pmf>::pattern>{}.params().size(),
				  "路由声明：Param<> 的个数必须等于路径里的命名捕获个数");

	check_all_param_names<Pmf>(std::make_index_sequence<Params::size()>{});
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

// 造一个注入实参：`Param<T>` 取第 slot 个捕获的值（整数用 from_chars，string_view 直通）；其余形参拿 request。
template <std::size_t index, class Pmf>
[[nodiscard]] decltype(auto) make_arg(const http::Request& request, std::span<const Capture> captures)
{
	using Params = typename RouteOf<Pmf>::Params;
	using P = typename Params::template At<index>;

	if constexpr(IsParam<P>::value)
	{
		constexpr auto slot = SlotOf<index, Params>::value;
		const std::string_view text = slot < captures.size() ? captures[slot].value : std::string_view{};
		using V = typename P::Value;
		V value{};

		if constexpr(std::is_same_v<V, std::string_view>)
			value = text;
		else if constexpr(std::is_integral_v<V>)
			std::from_chars(text.data(), text.data() + text.size(), value);
		else
			static_assert(!sizeof(V), "路由注入：Param<> 目前只支持整数与 string_view");

		return P{value};
	}
	else
	{
		return request;
	}
}

// 把成员函数指针包成 Handler：按自己的 pattern 匹配出捕获，再按形参表逐个造实参。
// 组的树只给出槽位，而擦除后的 `Handler` 没有携带捕获的通道 → 这里必须按本条 pattern 再匹配一次拿捕获。
template <class C, class F, std::size_t... indices>
[[nodiscard]] Handler make_handler(C* self, F pmf, std::index_sequence<indices...>)
{
	return [self, pmf](const http::Request& request) -> asio::awaitable<http::Response> {
		static constexpr auto tree = build<RouteOf<F>::pattern>();

		std::vector<Capture> captures;
		if(match(tree, request.path, captures) == k_no_route)
			co_return http::Response{500, "text/plain; charset=utf-8", {}, "路由派发：组命中但单条 pattern 未命中\n"};

		co_return co_await (self->*pmf)(make_arg<indices, F>(request, captures)...).release();
	};
}

// 一个动词下的路由：一棵该动词的编译期树 + 与之按槽位对齐的 handler 表。
// 动词必须分开建树：树只由 pattern 决定、动词不参与 —— 混在一棵树里的话
// `Get<"/x">` 与 `Post<"/x">` 会落到同一节点、互相覆盖槽位（后注册的吃掉先注册的）。
struct VerbRoutes
{
	std::string_view verb;
	std::function<std::uint32_t(std::string_view, std::vector<Capture>&)> match;
	std::vector<Handler> handlers;
};

// 挑出动词为 verb 的 handler；顺序与 SelectPatterns 选出的 pattern 一一对应。
template <Verb verb, class C, class... Pmf>
[[nodiscard]] std::vector<Handler> make_verb_handlers(C* self, Pmf... pmf)
{
	std::vector<Handler> handlers;
	handlers.reserve(sizeof...(Pmf));
	(void)std::initializer_list<int>{
		(RouteOf<Pmf>::verb == verb ?
			 (handlers.push_back(
				  make_handler(self, pmf, std::make_index_sequence<RouteOf<Pmf>::Params::size()>{})
			  ),
			  0) :
			 0)...
	};
	return handlers;
}

template <Verb verb, class C, class... Pmf>
void add_verb_routes(std::vector<VerbRoutes>& routes, C* self, Pmf... pmf)
{
	using patterns = typename SelectPatterns<verb, Pmf...>::type;
	if constexpr(!IsEmptyPatternList<patterns>::value)
	{
		static constexpr auto tree = build_of(patterns{});
		routes.push_back(
			VerbRoutes{to_string(verb), [](std::string_view path, std::vector<Capture>& out) { return match(tree, path, out); },
					   make_verb_handlers<verb>(self, pmf...)}
		);
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

export namespace std {

// 只多一行：把 promise 指向 asio 的 frame，于是 `Endpoint` 可以直接当协程返回类型。
// ⚠️ 必须写在**任何用到它的协程之前**（特化在协程定义点查找），且只能在全局 `std` 里。
template <pylai::route::Verb verb, pylai::route::FixedString pattern, class... Args>
struct coroutine_traits<pylai::route::Endpoint<verb, pattern>, Args...>
{
	using promise_type = asio::awaitable_frame<pylai::http::Response, asio::any_io_executor>;
};

} // namespace std

export namespace pylai::route {

// ───── 编译期用例 ─────────────────────────────────────────────────
// 具名常量，兼作**语法示例**；`static_assert` 在每次构建时验证。
namespace selftest {

// 精确匹配
inline constexpr Pattern<"/health"> health{};
inline constexpr Pattern<"/api/v1/echo"> echo{};

// 路径参数
inline constexpr Pattern<"/api/v1/users/{id}"> user_by_id{};
inline constexpr Pattern<"/api/v1/users/{id}/posts/{post_id}"> user_post{};

// 全捕获 / 子树（尾斜杠 = 匿名 `...`）
inline constexpr Pattern<"/files/{path...}"> files_tree{};
inline constexpr Pattern<"/static/"> static_tree{};

// 精确尾 `{$}`
inline constexpr Pattern<"/{$}"> root_only{};
inline constexpr Pattern<"/a/{$}"> a_only{};

} // namespace selftest

static_assert(selftest::health.count == 1 && selftest::health.kind(0) == SegKind::literal);
static_assert(selftest::echo.count == 3);
static_assert(selftest::user_by_id.count == 4 && selftest::user_by_id.text(3) == "id");
static_assert(selftest::user_post.count == 6 && selftest::user_post.text(5) == "post_id");
static_assert(selftest::files_tree.count == 2 && selftest::files_tree.kind(1) == SegKind::catch_all &&
			  selftest::files_tree.text(1) == "path");
static_assert(selftest::static_tree.count == 2 && selftest::static_tree.kind(1) == SegKind::catch_all);
static_assert(selftest::root_only.count == 1 && selftest::root_only.kind(0) == SegKind::end_mark);
static_assert(selftest::a_only.count == 2 && selftest::a_only.kind(1) == SegKind::end_mark);

// 根："/" → 匿名全捕获（匹配一切）
static_assert(Pattern<"/">{}.count == 1 && Pattern<"/">{}.kind(0) == SegKind::catch_all);
// 尾斜杠 = 子树
static_assert(Pattern<"/a/">{}.count == 2 && Pattern<"/a/">{}.kind(1) == SegKind::catch_all);

// —— 数组维度是**紧**的（segment_count = count('/')，name_count = count('{')），不再按 raw 长度过分配 ——
static_assert(Pattern<"/health">::segment_count == 1 && Pattern<"/health">::name_count == 0);
static_assert(Pattern<"/api/v1/echo">::segment_count == 3 && Pattern<"/api/v1/echo">::name_count == 0);
static_assert(Pattern<"/files/{path...}">::segment_count == 2 && Pattern<"/files/{path...}">::name_count == 1);
static_assert(Pattern<"/api/v1/users/{id}/posts/{post_id}">::segment_count == 6 &&
			  Pattern<"/api/v1/users/{id}/posts/{post_id}">::name_count == 2);
// `{$}` 是匿名的 → 会占一个 `{` 但**不进** names（多算一个维度，无妨）
static_assert(Pattern<"/a/{$}">::name_count == 1 && Pattern<"/a/{$}">{}.params().empty());

// —— 参数提取：按出现顺序，匿名占位符不计入 ——
static_assert(Pattern<"/health">{}.params().empty());
static_assert(Pattern<"/users/{id}">{}.params().size() == 1 && Pattern<"/users/{id}">{}.params()[0] == "id");
static_assert(Pattern<"/users/{id}/posts/{pid}">{}.params().size() == 2 &&
			  Pattern<"/users/{id}/posts/{pid}">{}.params()[1] == "pid");
static_assert(Pattern<"/files/{path...}">{}.params().size() == 1 &&
			  Pattern<"/files/{path...}">{}.params()[0] == "path");
static_assert(Pattern<"/">{}.params().empty());		// 匿名全捕获
static_assert(Pattern<"/a/">{}.params().empty());		// 尾斜杠子树
static_assert(Pattern<"/a/{$}">{}.params().empty());	// 精确尾
static_assert(selftest::user_post.params().size() == 2 && selftest::user_post.params()[0] == "id" &&
			  selftest::user_post.params()[1] == "post_id");

// ── DSL：成员指针抽取 + 位置注入 + 名字校对（协程本体由应用侧验证）──
namespace dsl_selftest {

struct User
{
	Get<"/health"> health(const http::Request&);
	Post<"/submit/{id}"> submit(Param<int, "id"> id, const http::Request&);
	Get<"/files/{path...}"> files(Param<std::string_view, "path"> path, const http::Request&);
};

using Health = RouteOf<decltype(&User::health)>;
using Submit = RouteOf<decltype(&User::submit)>;
using Files = RouteOf<decltype(&User::files)>;

static_assert(Health::verb == Verb::get && Health::pattern_view == "/health");
static_assert(Submit::verb == Verb::post && Submit::pattern_view == "/submit/{id}");
static_assert(Files::verb == Verb::get && Files::pattern_view == "/files/{path...}");

static_assert(ParamCount<Health::Params>::value == 0);
static_assert(ParamCount<Submit::Params>::value == 1);
static_assert(SlotOf<0, Submit::Params>::value == 0);
static_assert(SlotOf<1, Submit::Params>::value == 1); // request 占形参位，但不吃捕获
static_assert(std::is_same_v<typename Submit::Params::template At<0>, Param<int, "id">>);

// 换用 jk 的 `Callable` 白拿的能力：cv / noexcept 形式的成员指针也能当路由声明
// （旧的手写偏特化只匹配 `R (C::*)(Ps...)` —— 反证见 `.vscode/tmp/jk-callable-probe/old-trait.cpp`）。
struct QualifiedUser
{
	Get<"/c"> ping(const http::Request&) const;
	Post<"/n/{id}"> post(Param<int, "id"> id, const http::Request&) noexcept;
};

using ConstPing = RouteOf<decltype(&QualifiedUser::ping)>;
using NothrowPost = RouteOf<decltype(&QualifiedUser::post)>;

static_assert(ConstPing::verb == Verb::get && ConstPing::pattern_view == "/c");
static_assert(NothrowPost::verb == Verb::post && NothrowPost::pattern_view == "/n/{id}");
static_assert(SlotOf<1, NothrowPost::Params>::value == 1);

// 触发三条声明的编译期校验（Param 个数 / 名字 / pattern 合法性）
inline constexpr bool validated =
	(validate_route<decltype(&User::health)>(), validate_route<decltype(&User::submit)>(),
	 validate_route<decltype(&User::files)>(), true);
static_assert(validated);

// 强制实例化整条注册链（只求编译，不调用）：make_routes → make_handler → make_arg + build<...>()
inline void instantiate_chain()
{
	[[maybe_unused]] auto routes =
		make_routes(static_cast<User*>(nullptr), &User::health, &User::submit, &User::files);
	[[maybe_unused]] auto qualified =
		make_routes(static_cast<QualifiedUser*>(nullptr), &QualifiedUser::ping, &QualifiedUser::post);
}

} // namespace dsl_selftest

// —— 错误路径：以下每一条都必须被拦下 ——
static_assert(Pattern<"/a/{x...}/b">{}.err == ParseError::catch_all_not_last);
static_assert(Pattern<"/a/{x}/{x}">{}.err == ParseError::duplicate_param);
static_assert(Pattern<"/a//b">{}.err == ParseError::empty_segment);
static_assert(Pattern<"/a/{}">{}.err == ParseError::empty_param_name);
static_assert(Pattern<"/a/b{c}">{}.err == ParseError::orphan_brace);
static_assert(Pattern<"users/{id}">{}.err == ParseError::must_start_with_slash);

// —— 现状钉死（不是期望值，是"防止无意改变"的特征测试）——
// `{...}` 会落到普通参数分支，成为**名为 "..." 的参数**，而不是 Go 的匿名全捕获
static_assert(Pattern<"/a/{...}">{}.count == 2 && Pattern<"/a/{...}">{}.kind(1) == SegKind::param &&
			  Pattern<"/a/{...}">{}.text(1) == "...");
// `{{` / `}}` 转义**尚未实现**，字面量里写不出花括号：
//   `/a/b{c}`（`{` 不在段首）→ orphan_brace
//   `/a/{{b}}`（`{` 在段首、`}` 在段尾）→ 先当通配符，再因名字含 `{` → bad_param_name
static_assert(Pattern<"/a/b{c}">{}.err == ParseError::orphan_brace);
static_assert(Pattern<"/a/{{b}}">{}.err == ParseError::bad_param_name);

namespace conflict_selftest {

inline constexpr auto by_name = Pattern<"/api/v1/users/{name}">{};
inline constexpr auto by_id_posts = Pattern<"/api/v1/users/{id}/posts">{};
inline constexpr auto files_slash = Pattern<"/files/">{};
inline constexpr auto files_named = Pattern<"/files/{p...}">{};
inline constexpr auto wild_one = Pattern<"/a/{x}">{};
inline constexpr auto wild_rest = Pattern<"/a/{y...}">{};
inline constexpr auto diverge_param = Pattern<"/a/{x}/c">{};
inline constexpr auto diverge_literal = Pattern<"/a/b/{y}">{};
inline constexpr auto exact_end = Pattern<"/a/{$}">{};

// 指定 method 时两条路由是否冲突。
[[nodiscard]] constexpr bool conflicts_as(std::string_view method_a, PatternView a, std::string_view method_b, PatternView b)
{
	const std::string_view methods[] = {method_a, method_b};
	const PatternView both[] = {a, b};
	return !find_table_conflict(methods, both).ok();
}

[[nodiscard]] constexpr bool conflicts(PatternView a, PatternView b) { return conflicts_as("GET", a, "GET", b); }

static_assert(conflicts(selftest::user_by_id.view(), selftest::user_by_id.view())); // 完全重复
static_assert(conflicts(selftest::user_by_id.view(), by_name.view()));				// 同形异名
static_assert(conflicts(files_slash.view(), files_named.view()));					// /files/ 与 /files/{p...}
static_assert(conflicts(wild_one.view(), wild_rest.view()));						// 两个通配符抢同一段
static_assert(conflicts(wild_rest.view(), exact_end.view()));						// catch_all 抢走 {$} 的 "/"
static_assert(!conflicts(selftest::user_by_id.view(), by_id_posts.view()));			// 前缀相同但更长
static_assert(!conflicts(diverge_param.view(), diverge_literal.view()));			// 分歧点有字面量 → 放行
static_assert(!conflicts(wild_one.view(), exact_end.view()));						// param 捕不到空段

// 同 pattern、不同 method → 不是冲突（靠“每 method 一棵树”各走各的）
static_assert(!conflicts_as("GET", selftest::user_by_id.view(), "POST", selftest::user_by_id.view()));

} // namespace conflict_selftest

namespace match_selftest {

// 整棵树现在是**编译期常量**：节点数进类型 + 扁平数组 + 零堆分配。
// 槽位 = 下面 pattern 的出现顺序。
inline constexpr auto shared = build<
	"/health",
	"/api/v1/echo",
	"/api/v1/users/{id}",
	"/files/{path...}",
	"/a/{$}",
	"/api/v1/user",
	"/files/static">();

// 「先算」算出的精确规模 == 实际填满的规模（两边都得刚好用满）
static_assert(shared.node_count == shared.nodes.size());
static_assert(shared.edge_count == shared.edges.size());

[[nodiscard]] constexpr std::uint32_t hit(std::string_view path)
{
	std::vector<Capture> caps;
	return match(shared, path, caps);
}

// 压缩验收：/api/v1/echo 与 /api/v1/users/{id} 只在字符级共享前缀，
// 所以整棵树只需 5 个节点（root —"/api/v1/"→ 分叉点，分叉点再接 "echo" / "users"）。
inline constexpr auto compress_tree = build<"/api/v1/echo", "/api/v1/users/{id}">();
static_assert(compress_tree.node_count == compress_tree.nodes.size());

[[nodiscard]] constexpr bool compresses()
{
	if(compress_tree.node_count != 5) return false;

	for(const auto& e : compress_tree.edges)
		if(e.kind == RadixKind::chars && e.label == "/api/v1/") return true;

	return false;
}

// `/` 与 `{$}` 必须分开建树（`/` 与 `/{$}` 本身就冲突，而且 `/` 会吞掉所有 404 用例）。
inline constexpr auto root_any_tree = build<"/">();
inline constexpr auto root_end_tree = build<"/{$}", "/a/{$}">();

[[nodiscard]] constexpr std::uint32_t hit_root_any(std::string_view path)
{
	std::vector<Capture> caps;
	return match(root_any_tree, path, caps);
}

[[nodiscard]] constexpr std::uint32_t hit_root_end(std::string_view path)
{
	std::vector<Capture> caps;
	return match(root_end_tree, path, caps);
}

} // namespace match_selftest

static_assert(match_selftest::hit("/health") == 0);
static_assert(match_selftest::hit("/api/v1/echo") == 1);
static_assert(match_selftest::hit("/api/v1/users/42") == 2);
static_assert(match_selftest::hit("/api/v1/user") == 5);
static_assert(match_selftest::hit("/files/a/b/c") == 3);
static_assert(match_selftest::hit("/files/") == 3);
static_assert(match_selftest::hit("/a/") == 4);

// 静态优先：/files/static 羸过 /files/{path...}；其余段回退给 catch_all
static_assert(match_selftest::hit("/files/static") == 6);
static_assert(match_selftest::hit("/files/other") == 3);
// 回溯：字面量边走到了底、但路径没走完 → 让给 catch_all
static_assert(match_selftest::hit("/files/static/x") == 3);

// 下面每一条都应落空（404）
static_assert(match_selftest::hit("/files") == k_no_route);
static_assert(match_selftest::hit("/a") == k_no_route);
static_assert(match_selftest::hit("/api/v1/users/") == k_no_route);
static_assert(match_selftest::hit("/api/v1/users") == k_no_route);
static_assert(match_selftest::hit("/api/v1/users/42/posts") == k_no_route);
static_assert(match_selftest::hit("/health/") == k_no_route);
static_assert(match_selftest::hit("/nope") == k_no_route);

// 字符级压缩生效
static_assert(match_selftest::compresses());

// `/`：匿名全捕获，匹配一切（但空路径不行）
static_assert(match_selftest::hit_root_any("/") == 0);
static_assert(match_selftest::hit_root_any("/a/b") == 0);
static_assert(match_selftest::hit_root_any("") == k_no_route);

// `{$}`：只钉住“恰好到此为止”
static_assert(match_selftest::hit_root_end("/") == 0);
static_assert(match_selftest::hit_root_end("/a/") == 1);
static_assert(match_selftest::hit_root_end("/a") == k_no_route);
static_assert(match_selftest::hit_root_end("/a/b") == k_no_route);

static_assert([] {
	std::vector<Capture> caps;
	return match(match_selftest::shared, "/api/v1/users/42", caps) == 2 && caps.size() == 1 &&
		   caps[0].name == "id" && caps[0].value == "42";
}());

static_assert([] {
	std::vector<Capture> caps;
	return match(match_selftest::shared, "/files/a/b", caps) == 3 && caps.size() == 1 &&
		   caps[0].name == "path" && caps[0].value == "a/b";
}());

static_assert([] {
	std::vector<Capture> caps;
	return match(match_selftest::shared, "/files/", caps) == 3 && caps.size() == 1 &&
		   caps[0].value.empty();
}());

static_assert([] {
	std::vector<Capture> caps;
	return match(match_selftest::shared, "/files/static/x", caps) == 3 && caps.size() == 1 &&
		   caps[0].name == "path" && caps[0].value == "static/x";
}());

// FixedString::substr<Pos, End>()：半开区间 [Pos, End)
static_assert(FixedString{"abcdef"}.substr<1, 3>().view() == "bc");
static_assert(FixedString{"abcdef"}.substr<0, 6>().view() == "abcdef");
static_assert(FixedString{"abcdef"}.substr<3, 6>().view() == "def");
static_assert(FixedString{"abcdef"}.substr<2, 2>().view().empty());
static_assert(FixedString{"/api/v1/users"}.substr<0, 4>().view() == "/api");

// 关键：结果是**独立类型**，可以直接当 NTTP 交给 Pattern<...> —— 这正是
// `FixedString{string_view}` 做不到的事（探针 probe_fail.cpp 验过：推不出 N）。
// ⚠️ 但 pattern 必须先以 '/' 开头，所以要用 substr 递归时，切出来的子串**得带上前导 '/'**，
//    否则 Pattern 直接解析失败（count == 0）。
static_assert(Pattern<FixedString{"/a/{x}/b"}.substr<0, 6>()>{}.count == 2);
static_assert(Pattern<FixedString{"/a/{x}/b"}.substr<0, 6>()>{}.kind(1) == SegKind::param);
static_assert(Pattern<FixedString{"/api/v1/users"}.substr<0, 7>()>{}.count == 2);
static_assert(Pattern<FixedString{"/api/v1/users"}.substr<0, 7>()>{}.text(1) == "v1");
// 反例：切掉前导 '/' → 解析失败，count 为 0
static_assert(Pattern<FixedString{"/a/{x}/b"}.substr<1, 6>()>{}.count == 0);

} // namespace pylai::route
