#!/usr/bin/env python3
"""Source-extract the retry queue helpers and inject both container allocation failures."""
import argparse
from pathlib import Path
import subprocess


def extract(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    state, escaped, at = "code", False, brace
    while at < len(source):
        ch = source[at]
        nxt = source[at + 1] if at + 1 < len(source) else ""
        if state == "line":
            if ch == "\n": state = "code"
        elif state == "block":
            if ch == "*" and nxt == "/": state = "code"; at += 1
        elif state in ("string", "char"):
            if escaped: escaped = False
            elif ch == "\\": escaped = True
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"): state = "code"
        elif ch == "/" and nxt == "/": state = "line"; at += 1
        elif ch == "/" and nxt == "*": state = "block"; at += 1
        elif ch == '"': state = "string"
        elif ch == "'": state = "char"
        elif ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0: return source[start:at + 1]
        at += 1
    raise ValueError(f"unterminated function: {signature}")


def generate(root: Path, out: Path) -> Path:
    src = (root / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text()
    modern = "template<typename Up, typename Pending>\nbool deferResUpNoThrow("
    if modern in src:
        helper = extract(src, modern)
        reset = extract(src, "void LLCleanRendererVK::resetResUpQueueForRetry()")
        assert "deferResUpNoThrow(u, ups_left, mResUpRetryScan)" in src
        code = r'''#include <cassert>
#include <cstdint>
#include <new>
#include <vector>
#include <set>
struct Up { uint64_t key; };
struct Pending {
 std::vector<Up> rows; bool fail=false;
 void push_back(const Up& u) { if(fail) throw std::bad_alloc(); rows.push_back(u); }
};
''' + helper + r'''
struct LLCleanRendererVK {
 bool mResUpRetryScan=false,mResUpsUnsorted=true; uint32_t mResCursor=99;
 std::vector<Up> mResUps; std::set<uint64_t> mResUpQueued;
 void resetResUpQueueForRetry() noexcept;
};
''' + reset + r'''
int main(){
 Pending p; bool retry=false;
 assert(deferResUpNoThrow(Up{1},p,retry));
 p.fail=true; assert(!deferResUpNoThrow(Up{2},p,retry));
 assert(retry && p.rows.size()==1 && p.rows[0].key==1);
 LLCleanRendererVK r; r.mResUpRetryScan=retry;
 r.mResUps.push_back({1}); r.mResUpQueued.insert(1);
 r.resetResUpQueueForRetry();
 assert(!r.mResUpRetryScan && r.mResCursor==0 && r.mResUps.empty() && r.mResUpQueued.empty() && !r.mResUpsUnsorted);
 p.fail=false; assert(deferResUpNoThrow(Up{2},p,retry));
 assert(p.rows.size()==2 && p.rows.back().key==2);
 r.mResCursor=17; r.resetResUpQueueForRetry(); assert(r.mResCursor==17);
}
'''
        out.mkdir(parents=True, exist_ok=True)
        path = out / "texture_resup_oom_checks.cpp"
        path.write_text(code)
        return path
    helper = extract(src, "template<typename Up, typename Queued, typename Pending>\nbool deferResUpNoThrow(")
    reset = extract(src, "void LLCleanRendererVK::resetResUpQueueForRetry()")
    branch = src[src.index("if (stageResult != TexStageStartResult::Ineligible)"):]
    branch = branch[:branch.index("const uint32_t tag =")]
    assert "deferResUpNoThrow(u, mResUpQueued, ups_left, mResUpRetryScan)" in branch
    assert "batch.add(" not in branch and "swapShared(" not in branch
    pending = src[src.index("if (ticket_pending && !ticket_ready)"):]
    pending = pending[:pending.index("bool deferReadyTicket")]
    assert "deferResUpNoThrow(u, mResUpQueued, ups_left, mResUpRetryScan)" in pending
    upload = src[src.index("uint32_t LLCleanRendererVK::uploadRealMaterialTextures("):]
    assert upload.index("resetResUpQueueForRetry();") < upload.index("if (mResCursor == 0)")

    cpp = r'''#include <cassert>
#include <cstdint>
#include <new>
#include <set>
#include <vector>
struct Up { uint32_t prio=0; float ratio=1.f; uint64_t key=0; };
struct ThrowSet {
 std::set<uint64_t> data; bool failInsert=false;
 void erase(uint64_t key) noexcept { data.erase(key); }
 std::pair<std::set<uint64_t>::iterator,bool> insert(uint64_t key) {
  if(failInsert){failInsert=false;throw std::bad_alloc();} return data.insert(key);
 }
 void clear() noexcept { data.clear(); }
};
struct ThrowVector {
 std::vector<Up> data; bool failPush=false;
 void push_back(const Up& up) { if(failPush){failPush=false;throw std::bad_alloc();} data.push_back(up); }
};
'''
    cpp += helper + "\n\n"
    cpp += "class LLCleanRendererVK { public: std::vector<Up> mResUps; ThrowSet mResUpQueued; " \
           "bool mResUpRetryScan=false,mResUpsUnsorted=true; uint32_t mResCursor=99; void resetResUpQueueForRetry() noexcept; };\n"
    cpp += reset + r'''
static void insertion_oom_rolls_back_and_rescans() {
 Up u{1,2.f,55};ThrowSet q;q.data.insert(55);q.failInsert=true;ThrowVector v;bool retry=false;
 assert(!deferResUpNoThrow(u,q,v,retry));assert(q.data.empty());assert(v.data.empty());assert(retry);
 LLCleanRendererVK r;r.mResUpRetryScan=retry;r.mResCursor=44;r.mResUps.push_back({0,1.f,8});r.mResUpQueued.data.insert(8);
 r.resetResUpQueueForRetry();assert(!r.mResUpRetryScan&&r.mResCursor==0&&r.mResUps.empty()&&r.mResUpQueued.data.empty()&&!r.mResUpsUnsorted);
 ThrowVector recovered;assert(deferResUpNoThrow(u,r.mResUpQueued,recovered,r.mResUpRetryScan));
 assert(recovered.data.size()==1&&r.mResUpQueued.data.count(55)==1);
}
static void pending_vector_oom_rolls_back_and_rescans() {
 Up u{0,1.f,91};ThrowSet q;q.data.insert(91);ThrowVector v;v.failPush=true;bool retry=false;
 assert(!deferResUpNoThrow(u,q,v,retry));assert(q.data.empty());assert(v.data.empty());assert(retry);
 LLCleanRendererVK r;r.mResUpRetryScan=retry;r.mResCursor=2;r.resetResUpQueueForRetry();
 assert(!r.mResUpRetryScan&&r.mResCursor==0);
 ThrowVector recovered;assert(deferResUpNoThrow(u,r.mResUpQueued,recovered,r.mResUpRetryScan));
 assert(recovered.data.size()==1&&r.mResUpQueued.data.count(91)==1);
}
int main(){insertion_oom_rolls_back_and_rescans();pending_vector_oom_rolls_back_and_rescans();}
'''
    out.mkdir(parents=True, exist_ok=True)
    path = out / "texture_resup_oom_checks.cpp"
    path.write_text(cpp)
    return path


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--run", action="store_true")
    a = ap.parse_args()
    path = generate(a.source_root.resolve(), a.out.resolve())
    print(path)
    if a.run:
        exe = path.with_suffix("")
        subprocess.run(["/usr/bin/c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
                        str(path), "-o", str(exe)], check=True, cwd=a.out.resolve())
        subprocess.run([str(exe)], check=True, cwd=a.out.resolve())
        print("PASS: set/vector allocation failures roll back and trigger allocation-free rescan")


if __name__ == "__main__":
    main()
