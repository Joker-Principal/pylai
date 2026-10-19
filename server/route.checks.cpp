// 路由的**编译期自检**：`static_assert` 每次构建都求值 —— 构建过 = 用例过（证据纪律见 AGENTS.md §2）。
//
// 为什么住在库里（而不是 `tests/`）：这些断言要看树、建树器、匹配器、`FixedString` 这些**不导出**的内部件
// （导出面只留使用者需要的东西，见 docs/coding-discipline.md）。导出面另有护栏：`tests/main.cpp` 走
// `import pylai;` 的公开面 + e2e 全程跑。
// ⚠️ “必须编不过”的失败路径不在这里 —— 那要靠人工探针（见 docs/http-server-plan.md §11.2）。
module;
#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <vector>

module pylai;

import :http;
import :route;

using namespace pylai::route;

// ───── 具名 pattern 常量（兼作语法示例）─────────────────────────────────
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

// ───── DSL：成员指针抽取 + 声明住在第一个形参 + `param<T, "名字">()` ─────
namespace dsl_selftest {

struct User
{
public:
	asio::awaitable<void> health(Get<"/health"> req, pylai::route::StringResponse& res);
	asio::awaitable<void> submit(Post<"/submit/{id}"> req, pylai::route::StringResponse& res);
	asio::awaitable<void> files(Get<"/files/{path...}"> req, pylai::route::EmptyResponse& res); // 响应形参是必需的
};

using Health = RouteOf<decltype(&User::health)>;
using Submit = RouteOf<decltype(&User::submit)>;
using Files = RouteOf<decltype(&User::files)>;

static_assert(Health::verb == Verb::get && Health::pattern_view == "/health");
static_assert(Submit::verb == Verb::post && Submit::pattern_view == "/submit/{id}");
static_assert(Files::verb == Verb::get && Files::pattern_view == "/files/{path...}");

static_assert(IsRouteRequest<typename Submit::Params::template At<0>>::value); // 第 0 个形参是请求
static_assert(std::is_same_v<typename Submit::Params::template At<1>, pylai::route::StringResponse&>); // 响应按引用

// 命名捕获的**下标**在编译期算出来 —— 运行期只按这个下标取值，不按名字查找
static_assert(Request<Verb::post, "/submit/{id}">::slot_of("id") == 0);

// 挂点（plan §4.1.4）：类别指纹必须**稳定**且**区分得开** —— 同一个 path 的不同动词不能撞。
static_assert(Request<Verb::get, "/x">::fingerprint() != Request<Verb::post, "/x">::fingerprint());
static_assert(Request<Verb::get, "/x">::fingerprint() != Request<Verb::get, "/y">::fingerprint());
static_assert(Request<Verb::get, "/x">::fingerprint() == Request<Verb::get, "/x">::fingerprint()); // 稳定 ⇒ 同一声明同值
#if PYLAI_ENABLE_OBSERVATION
static_assert(!std::is_empty_v<pylai::http::Observation>); // 默认开启 ⇒ 观测槽有字段
#endif
static_assert(Request<Verb::get, "/files/{path...}">::slot_of("path") == 0);
static_assert(Request<Verb::get, "/users/{id}/posts/{pid}">::slot_of("pid") == 1);
// 没这个名字 → 返回 `params().size()`（越界；`param()` 正是拿它拦错的）
static_assert(Request<Verb::post, "/submit/{id}">::slot_of("nope") == Pattern<"/submit/{id}">{}.params().size());

// `param<T, "名字">()` 本体**不在编译期验**：`Request` 持有指向运行期报文数据的引用 / span，
// 不是字面量类型，无法常量求值。行为由 `tests/` 的端到端用例覆盖：
// `/users/{id}` 同时走 int 与 string_view 两条路，`/users/abc` 验转换失败静默给 0。

// 换用 jk 的 `Callable` 白拿的能力：cv / noexcept 形式的成员指针也能当路由声明
// （旧的手写偏特化只匹配 `R (C::*)(Ps...)` —— 反证见 `.vscode/tmp/jk-callable-probe/old-trait.cpp`）。
struct QualifiedUser
{
public:
	asio::awaitable<void> ping(Get<"/c"> req, pylai::route::StringResponse& res) const;
	asio::awaitable<void> post(Post<"/n/{id}"> req, pylai::route::StringResponse& res) noexcept;
};

using ConstPing = RouteOf<decltype(&QualifiedUser::ping)>;
using NothrowPost = RouteOf<decltype(&QualifiedUser::post)>;

static_assert(ConstPing::verb == Verb::get && ConstPing::pattern_view == "/c");
static_assert(NothrowPost::verb == Verb::post && NothrowPost::pattern_view == "/n/{id}");

// 触发三条声明的编译期校验（pattern 合法性）
inline constexpr bool validated =
	(validate_route<decltype(&User::health)>(), validate_route<decltype(&User::submit)>(),
	 validate_route<decltype(&User::files)>(), true);
static_assert(validated);

// 强制实例化整条注册链（只求编译，不调用）：make_routes → make_handler → make_arg + build<...>()
// 这一段同时是“消费方能不能实例化这些模板”的验收（当初 `awaitable_frame` 没导出就是在这里暴露的）。
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
