#!/usr/bin/env python3
"""
confluence_upload.py — cgreclaim 벤치마크 분석 결과를 Confluence에 업로드

페이지 구조:
    [parent] cgreclaim 분석 YYYY-MM-DD [session]   ← 요약만
        ├── 메모리 분석 [session]                   ← meminfo 상세
        ├── Cgroup 분석 [session]                   ← cgroup 상세
        └── Syslog 타이밍 분석 [session]            ← syslog 상세

Usage:
    python3 confluence_upload.py --user <id> --password <pw>
    python3 confluence_upload.py --user <id> --password <pw> \\
        --session 20260408_224918 --parent-page-id 3614431987

환경변수:
    CONFLUENCE_USER, CONFLUENCE_PASS, CONFLUENCE_PARENT_PAGE_ID
"""

import argparse
import csv
import os
import re
import statistics
import sys
from datetime import datetime
from pathlib import Path

import requests
from requests.auth import HTTPBasicAuth

HERE = Path(__file__).parent.resolve()
TOOLS_DIR = HERE.parent
DEFAULT_DATA_DIR = TOOLS_DIR / "data"
DEFAULT_REPORT_DIR = TOOLS_DIR / "report"

CONFLUENCE_BASE = "http://collab.lge.com/main"
DEFAULT_PARENT_PAGE_ID = os.environ.get("CONFLUENCE_PARENT_PAGE_ID", "3614431987")


# ── Confluence API helpers ────────────────────────────────────────────────────

def check_auth(s: requests.Session) -> str:
    r = s.get(f"{CONFLUENCE_BASE}/rest/api/user/current", timeout=10)
    r.raise_for_status()
    info = r.json()
    if info.get("type") == "anonymous":
        raise RuntimeError("Authentication failed — anonymous user")
    return info.get("displayName", info.get("username", "?"))


def get_space_key(s: requests.Session, parent_page_id: str) -> str:
    r = s.get(f"{CONFLUENCE_BASE}/rest/api/content/{parent_page_id}",
              params={"expand": "space"}, timeout=10)
    r.raise_for_status()
    return r.json()["space"]["key"]


def get_or_create_page(s: requests.Session, parent_id: str, space_key: str,
                       title: str, body: str) -> str:
    """Return page_id: create new or move+update existing page under parent_id."""
    r = s.get(f"{CONFLUENCE_BASE}/rest/api/content",
              params={"title": title, "spaceKey": space_key,
                      "expand": "version,ancestors"}, timeout=10)
    existing = r.json().get("results", [])
    if existing:
        page = existing[0]
        page_id = page["id"]
        # Re-parent if the page is under the wrong parent
        current_parent = page.get("ancestors", [{}])[-1].get("id") if page.get("ancestors") else None
        if current_parent != str(parent_id):
            ver = page["version"]["number"]
            payload = {
                "version": {"number": ver + 1},
                "title": title,
                "type": "page",
                "ancestors": [{"id": parent_id}],
                "body": {"storage": {"value": "<p>업데이트 중...</p>", "representation": "storage"}},
            }
            s.put(f"{CONFLUENCE_BASE}/rest/api/content/{page_id}",
                  json=payload, timeout=15)
        else:
            update_page_content(s, page_id, "<p>업데이트 중...</p>")
        return page_id
    return create_child_page(s, parent_id, space_key, title, "<p>업로드 중...</p>")


def create_child_page(s: requests.Session, parent_id: str, space_key: str,
                      title: str, body: str) -> str:
    """Create a new child page; return new page ID."""
    payload = {
        "type": "page",
        "title": title,
        "space": {"key": space_key},
        "ancestors": [{"id": parent_id}],
        "body": {
            "storage": {
                "value": body,
                "representation": "storage"
            }
        }
    }
    r = s.post(
        f"{CONFLUENCE_BASE}/rest/api/content",
        json=payload,
        headers={"Content-Type": "application/json"},
        timeout=30
    )
    if not r.ok:
        print(f"ERROR: {r.status_code} {r.text[:800]}")
        r.raise_for_status()
    page_id = r.json()["id"]
    print(f"  ✓ Created: '{title}' (id={page_id})")
    return page_id


def update_page_content(s: requests.Session, page_id: str, body: str):
    """Update body of an existing page (increment version)."""
    r = s.get(f"{CONFLUENCE_BASE}/rest/api/content/{page_id}",
              params={"expand": "version,title"}, timeout=10)
    r.raise_for_status()
    info = r.json()
    version = info["version"]["number"]
    title = info["title"]

    payload = {
        "id": page_id,
        "type": "page",
        "title": title,
        "version": {"number": version + 1},
        "body": {"storage": {"value": body, "representation": "storage"}}
    }
    r = s.put(
        f"{CONFLUENCE_BASE}/rest/api/content/{page_id}",
        json=payload,
        headers={"Content-Type": "application/json"},
        timeout=30
    )
    if not r.ok:
        print(f"ERROR: {r.status_code} {r.text[:500]}")
        r.raise_for_status()
    print(f"  ✓ Page updated (v{version + 1}): '{title}'")


def upload_attachment(s: requests.Session, page_id: str, img_path: Path) -> bool:
    """Upload PNG as attachment to page_id. Returns True on success."""
    if not img_path.exists():
        print(f"  ⚠ Skipped (not found): {img_path.name}")
        return False
    url = f"{CONFLUENCE_BASE}/rest/api/content/{page_id}/child/attachment"
    r = s.get(url, params={"filename": img_path.name}, timeout=15)
    existing = r.json().get("results", [])
    headers = {"X-Atlassian-Token": "no-check"}
    with open(img_path, "rb") as f:
        files = {"file": (img_path.name, f, "image/png")}
        if existing:
            att_id = existing[0]["id"]
            r = s.post(f"{url}/{att_id}/data", headers=headers, files=files, timeout=30)
        else:
            r = s.post(url, headers=headers, files=files,
                       params={"allowDuplicate": "false"}, timeout=30)
    if not r.ok:
        print(f"  ⚠ Failed to upload {img_path.name}: {r.status_code}")
        return False
    print(f"  ✓ Uploaded: {img_path.name}")
    return True


# ── Markdown → Confluence Storage helpers ────────────────────────────────────

def md_table_to_html(md: str) -> str:
    lines = [l for l in md.strip().split("\n") if l.strip()]
    if len(lines) < 2:
        return f"<p>{md}</p>"
    html = "<table class=\"wrapped\"><tbody>"
    first = True
    for line in lines:
        if re.match(r"^\|[-| :]+\|$", line.strip()):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        tag = "th" if first else "td"
        html += "<tr>" + "".join(f"<{tag}>{c}</{tag}>" for c in cells) + "</tr>"
        first = False
    html += "</tbody></table>"
    return html


def md_to_storage(text: str) -> str:
    """Convert basic markdown to Confluence Storage Format."""
    lines = text.split("\n")
    out = []
    in_code = False
    code_buf = []
    table_buf = []

    def flush_table():
        if table_buf:
            out.append(md_table_to_html("\n".join(table_buf)))
            table_buf.clear()

    for line in lines:
        if line.startswith("```"):
            if not in_code:
                flush_table()
                in_code = True
                code_buf = []
            else:
                in_code = False
                lang = "bash"
                code_text = "\n".join(code_buf)
                out.append(
                    f'<ac:structured-macro ac:name="code">'
                    f'<ac:parameter ac:name="language">{lang}</ac:parameter>'
                    f'<ac:rich-text-body>{code_text}</ac:rich-text-body>'
                    f'</ac:structured-macro>'
                )
            continue
        if in_code:
            code_buf.append(line)
            continue
        if line.startswith("|"):
            table_buf.append(line)
            continue
        else:
            flush_table()
        if line.startswith("#### "):
            out.append(f"<h4>{line[5:]}</h4>")
        elif line.startswith("### "):
            out.append(f"<h3>{line[4:]}</h3>")
        elif line.startswith("## "):
            out.append(f"<h2>{line[3:]}</h2>")
        elif line.startswith("# "):
            out.append(f"<h1>{line[2:]}</h1>")
        elif line.startswith("- ") or line.startswith("* "):
            out.append(f"<li>{line[2:]}</li>")
        elif line.startswith("> "):
            out.append(f"<blockquote><p>{line[2:]}</p></blockquote>")
        elif line.strip() == "" or line.strip() == "---":
            out.append("<p/>")
        else:
            l = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", line)
            l = re.sub(r"`(.+?)`", r"<code>\1</code>", l)
            out.append(f"<p>{l}</p>")

    flush_table()
    return "\n".join(out)


def img_macro(filename: str, caption: str, width: int = 900) -> str:
    return (
        f'<p><strong>{caption}</strong></p>'
        f'<p><ac:image ac:align="center" ac:width="{width}">'
        f'<ri:attachment ri:filename="{filename}"/>'
        f'</ac:image></p>'
    )


def info_panel(title: str, content: str) -> str:
    return (
        f'<ac:structured-macro ac:name="info" ac:schema-version="1">'
        f'<ac:parameter ac:name="title">{title}</ac:parameter>'
        f'<ac:rich-text-body>{content}</ac:rich-text-body>'
        f'</ac:structured-macro>'
    )


def data_path_macro(session_id: str) -> str:
    content = (f"Session : {session_id}\n"
               f"Data    : ~/git/cgreclaim/tools/data/{session_id}/\n"
               f"Report  : ~/git/cgreclaim/tools/report/{session_id}/")
    return (
        f'<h2>📁 데이터 경로</h2>'
        f'<ac:structured-macro ac:name="code" ac:schema-version="1">'
        f'<ac:parameter ac:name="language">bash</ac:parameter>'
        f'<ac:plain-text-body><![CDATA[{content}]]></ac:plain-text-body>'
        f'</ac:structured-macro>'
        f'<p><em>Generated by <code>tools/analyze/confluence_upload.py</code></em></p>'
    )


# ── Data analysis helpers ─────────────────────────────────────────────────────

def load_meminfo_rows(data_dir: Path, group: str) -> list:
    rows = []
    for f in sorted(data_dir.glob(f"{group}/run_*/meminfo_sampler/*.csv")):
        with open(f) as fh:
            for r in csv.DictReader(fh):
                rows.append(r)
    return rows


def meminfo_phase_stats(rows: list, phase: int) -> dict:
    m = {"MemFree": [], "MemAvailable": [], "SwapUsed": [],
         "ActiveAnon": [], "InactiveAnon": []}
    for r in rows:
        if int(r["phase"]) == phase:
            st = float(r["swap_total_kb"]); sf = float(r["swap_free_kb"])
            m["MemFree"].append(float(r["mem_free_kb"]) / 1024)
            m["MemAvailable"].append(float(r["mem_available_kb"]) / 1024)
            m["SwapUsed"].append((st - sf) / 1024)
            m["ActiveAnon"].append(float(r["active_anon_kb"]) / 1024)
            m["InactiveAnon"].append(float(r["inactive_anon_kb"]) / 1024)
    return {k: {"mean": statistics.mean(v),
                "std": statistics.stdev(v) if len(v) > 1 else 0}
            for k, v in m.items() if v}


def load_cgroup_rates(data_dir: Path, group: str) -> dict:
    data: dict = {}
    for f in sorted(data_dir.glob(f"{group}/run_*/cgroup_sampler/*.csv")):
        cg = Path(f).stem
        if cg not in data:
            data[cg] = {"mem_mb": [], "pgmajfault_rate": [], "psi_some_avg10": []}
        with open(f) as fh:
            for r in csv.DictReader(fh):
                for col in data[cg]:
                    if col in r and r[col]:
                        data[cg][col].append(float(r[col]))
    return {k: {col: statistics.mean(v) for col, v in cols.items() if v}
            for k, cols in data.items()}


def mem_trow(metric: str, w: dict, wo: dict, higher_is_better: bool = True) -> str:
    delta = w["mean"] - wo["mean"]
    good = delta > 1 if higher_is_better else delta < -1
    icon = "✅" if good else ("🔴" if abs(delta) > 20 else "⚠️")
    return (f'<tr><td>{metric}</td>'
            f'<td>{w["mean"]:.1f} MB</td><td>{wo["mean"]:.1f} MB</td>'
            f'<td>{delta:+.1f} MB</td>'
            f'<td>±{w["std"]:.1f}</td><td>±{wo["std"]:.1f}</td>'
            f'<td>{icon}</td></tr>')


THEAD = ('<table class="wrapped"><tbody>'
         '<tr><th>Metric</th><th>with_cgrd mean</th><th>without_cgrd mean</th>'
         '<th>Δ (with−without)</th><th>with std</th><th>without std</th><th></th></tr>')


# ── Page body builders ────────────────────────────────────────────────────────

def build_summary_body(session_id: str, has_syslog: bool, has_memory: bool,
                       child_pages: dict, now_str: str) -> str:
    """Parent date page: overview + links to child pages."""
    items = ""
    for title, pid in child_pages.items():
        url = f"{CONFLUENCE_BASE}/pages/viewpage.action?pageId={pid}"
        items += f'<li><a href="{url}">{title}</a></li>'

    collected = []
    if has_syslog:
        collected.append("⏱ Syslog 타이밍")
    if has_memory:
        collected.append("🧠 메모리 프로파일링 (meminfo + cgroup)")
    collected_str = ", ".join(collected) if collected else "없음"

    return f"""
{info_panel("cgreclaim 벤치마크 분석 보고서",
    f"<p><strong>Session:</strong> <code>{session_id}</code><br/>"
    f"<strong>생성:</strong> {now_str}<br/>"
    f"<strong>시나리오:</strong> YouTube → Netflix 앱 전환 (with_cgrd vs without_cgrd)<br/>"
    f"<strong>수집 항목:</strong> {collected_str}</p>"
)}
<h2>분석 문서 목록</h2>
<ul>{items}</ul>
{data_path_macro(session_id)}
"""


def build_meminfo_body(session_id: str, data_dir: Path,
                       report_dir: Path, has_png: bool) -> str:
    runs_w = len(list(data_dir.glob("with_cgrd/run_*")))
    runs_wo = len(list(data_dir.glob("without_cgrd/run_*")))

    with_rows = load_meminfo_rows(data_dir, "with_cgrd")
    wo_rows = load_meminfo_rows(data_dir, "without_cgrd")

    if not with_rows or not wo_rows:
        return info_panel("데이터 없음", "<p>meminfo CSV 파일을 찾을 수 없습니다.</p>")

    p1w = meminfo_phase_stats(with_rows, 1); p1wo = meminfo_phase_stats(wo_rows, 1)
    p2w = meminfo_phase_stats(with_rows, 2); p2wo = meminfo_phase_stats(wo_rows, 2)

    def summary_panel_title(p2w, p2wo):
        """Generate data-driven panel title from Phase 2 (Netflix) deltas."""
        parts = []
        for m in ["MemFree", "MemAvailable", "SwapUsed"]:
            if m in p2w and m in p2wo:
                d = p2w[m]["mean"] - p2wo[m]["mean"]
                if m == "MemFree":
                    parts.append(f"MemFree {d:+.0f}MB")
                elif m == "SwapUsed":
                    parts.append(f"Swap {d:+.0f}MB")
        return "핵심 결론 (Phase 2 Netflix): " + " | ".join(parts) if parts else "핵심 결론"

    def tradeoff_rows(p1w, p1wo, p2w, p2wo):
        """Generate data-driven trade-off rows from actual deltas."""
        rows = []
        # Check each metric across phases (use Phase 2 Netflix as primary)
        checks = [
            ("MemFree",      True,   "MemFree",      "앱 전환 시 가용 메모리"),
            ("MemAvailable", True,   "MemAvailable", "실제 할당 가능 메모리"),
            ("SwapUsed",     False,  "Swap 사용량",  "reclaim된 페이지가 swap으로 이동 (swap I/O 비용 증가)"),
            ("ActiveAnon",   False,  "Active Anon",  "활성 익명 메모리 (앱 실행 중 사용)"),
            ("InactiveAnon", False,  "Inactive Anon","비활성 익명 메모리 (cgrd reclaim 대상)"),
        ]
        for key, higher_is_better, label, desc in checks:
            w2 = p2w.get(key, {})
            wo2 = p2wo.get(key, {})
            if not w2 or not wo2:
                continue
            d = w2["mean"] - wo2["mean"]
            # Positive delta = with_cgrd has more of this metric
            improved = d > 1 if higher_is_better else d < -1
            worsened = (d < -5 if higher_is_better else d > 5)
            if improved:
                icon, eval_str = "✅", "긍정적"
            elif worsened:
                icon, eval_str = "🔴", "부정적"
            else:
                icon, eval_str = "⚠️", "중립/주의"
            rows.append(f'<tr><td>{label} ({d:+.0f} MB)</td><td>{icon} {eval_str}</td><td>{desc}</td></tr>')
        # Note about pgmajfault/PSI — sourced from cgroup page
        rows.append('<tr><td>pgmajfault / PSI</td><td>→ Cgroup 분석 페이지 참조</td><td>cgroup별 major fault rate 및 pressure 지표</td></tr>')
        return "\n  ".join(rows)

    def summary_rows():
        html = ""
        for lbl, pw, pwo in [("Phase 1 (YouTube)", p1w, p1wo),
                               ("Phase 2 (Netflix)", p2w, p2wo)]:
            for m in ["MemFree", "MemAvailable", "SwapUsed"]:
                if m not in pw or m not in pwo:
                    continue
                d = pw[m]["mean"] - pwo[m]["mean"]
                html += (f'<tr><td>{lbl}</td><td>{m}</td>'
                         f'<td>{pw[m]["mean"]:.1f} MB</td>'
                         f'<td>{pwo[m]["mean"]:.1f} MB</td>'
                         f'<td><strong>{d:+.1f} MB</strong></td></tr>')
        return html

    p1_rows = ""
    for metric, w, wo, hib in [
        ("MemFree",       p1w.get("MemFree",{}),       p1wo.get("MemFree",{}),       True),
        ("MemAvailable",  p1w.get("MemAvailable",{}),  p1wo.get("MemAvailable",{}),  True),
        ("SwapUsed",      p1w.get("SwapUsed",{}),       p1wo.get("SwapUsed",{}),      False),
        ("Active Anon",   p1w.get("ActiveAnon",{}),     p1wo.get("ActiveAnon",{}),    False),
        ("Inactive Anon", p1w.get("InactiveAnon",{}),   p1wo.get("InactiveAnon",{}),  False),
    ]:
        if w and wo:
            p1_rows += mem_trow(metric, w, wo, hib)

    p2_rows = ""
    for metric, w, wo, hib in [
        ("MemFree",       p2w.get("MemFree",{}),       p2wo.get("MemFree",{}),       True),
        ("MemAvailable",  p2w.get("MemAvailable",{}),  p2wo.get("MemAvailable",{}),  True),
        ("SwapUsed",      p2w.get("SwapUsed",{}),       p2wo.get("SwapUsed",{}),      False),
        ("Active Anon",   p2w.get("ActiveAnon",{}),     p2wo.get("ActiveAnon",{}),    False),
        ("Inactive Anon", p2w.get("InactiveAnon",{}),   p2wo.get("InactiveAnon",{}),  False),
    ]:
        if w and wo:
            p2_rows += mem_trow(metric, w, wo, hib)

    png_block = img_macro("meminfo_comparison.png",
                          "Phase 1 / Phase 2 — MemFree / MemAvail / Swap / Anon 시계열") if has_png else ""

    return f"""
{info_panel("cgreclaim 메모리 분석 보고서",
    f"<p>Session: <code>{session_id}</code> | "
    f"with_cgrd {runs_w}회 vs without_cgrd {runs_wo}회 | "
    f"YouTube→Netflix 앱 전환 시나리오</p>"
)}
<h2>Executive Summary</h2>
<ac:structured-macro ac:name="panel" ac:schema-version="1">
  <ac:parameter ac:name="title">{summary_panel_title(p2w, p2wo)}</ac:parameter>
  <ac:rich-text-body>
    <table class="wrapped"><tbody>
      <tr><th>Phase</th><th>Metric</th><th>with_cgrd</th>
          <th>without_cgrd</th><th>Δ (with−without)</th></tr>
      {summary_rows()}
    </tbody></table>
    <p><em>✅ Positive Δ = with_cgrd가 더 많은 free memory / 적은 swap (개선)</em><br/>
    <em>⚠️ Negative Δ = with_cgrd가 더 많은 메모리 사용 (주의)</em></p>
  </ac:rich-text-body>
</ac:structured-macro>
<h2>Phase 1 — YouTube 실행 중</h2>
{THEAD}{p1_rows}</tbody></table>
<h2>Phase 2 — Netflix 실행 중</h2>
{THEAD}{p2_rows}</tbody></table>
{png_block}
<h2>Trade-off 분석</h2>
<table class="wrapped"><tbody>
  <tr><th>항목 (Phase 2 Δ)</th><th>평가</th><th>설명</th></tr>
  {tradeoff_rows(p1w, p1wo, p2w, p2wo)}
</tbody></table>
{data_path_macro(session_id)}
"""


def build_cgroup_body(session_id: str, data_dir: Path, has_png: bool) -> str:
    runs_w = len(list(data_dir.glob("with_cgrd/run_*")))
    runs_wo = len(list(data_dir.glob("without_cgrd/run_*")))

    with_cg = load_cgroup_rates(data_dir, "with_cgrd")
    wo_cg = load_cgroup_rates(data_dir, "without_cgrd")

    if not with_cg or not wo_cg:
        return info_panel("데이터 없음", "<p>cgroup CSV 파일을 찾을 수 없습니다.</p>")

    all_cg = set(with_cg) | set(wo_cg)
    cg_rows = []
    for cg in all_cg:
        w = with_cg.get(cg, {})
        o = wo_cg.get(cg, {})
        wmem = w.get("mem_mb", 0); omem = o.get("mem_mb", 0)
        cg_rows.append((cg, wmem, omem, wmem - omem,
                        w.get("pgmajfault_rate", 0), o.get("pgmajfault_rate", 0),
                        w.get("psi_some_avg10", 0), o.get("psi_some_avg10", 0)))
    cg_rows.sort(key=lambda x: abs(x[3]), reverse=True)

    top10_html = ""
    for r in cg_rows[:10]:
        name = r[0].replace("system.slice_", "").replace(".service", "").replace(".scope", "")
        d = r[3]
        icon = "✅" if d < -1 else ("🔴" if d > 1 else "—")
        top10_html += (f'<tr><td>{name}</td>'
                       f'<td>{r[1]:.1f} MB</td><td>{r[2]:.1f} MB</td>'
                       f'<td>{d:+.1f} MB {icon}</td>'
                       f'<td>{r[4]:.3f}</td><td>{r[5]:.3f}</td>'
                       f'<td>{r[6]:.2f}</td><td>{r[7]:.2f}</td></tr>')

    # PSI top-5
    psi_rows = ""
    for r in sorted(cg_rows[:10], key=lambda x: x[6], reverse=True)[:5]:
        name = r[0].replace("system.slice_", "").replace(".service", "")
        ratio = f"×{int(r[6]/r[7])}" if r[7] > 0.01 else "—"
        psi_rows += (f'<tr><td>{name}</td>'
                     f'<td>{r[6]:.2f}%</td><td>{r[7]:.2f}%</td><td>{ratio}</td></tr>')

    png_block = img_macro("cgroup_comparison.png",
                          "cgroup 메모리 / PSI 비교 — 전체 런 오버레이") if has_png else ""

    return f"""
{info_panel("cgreclaim Cgroup 메모리 분석 보고서",
    f"<p>Session: <code>{session_id}</code> | "
    f"with_cgrd {runs_w}회 vs without_cgrd {runs_wo}회</p>"
)}
<h2>Cgroup 메모리 비교 (Top 10 — |Δ| 기준)</h2>
<table class="wrapped"><tbody>
  <tr><th>cgroup</th><th>with_cgrd mem</th><th>without_cgrd mem</th><th>Δ mem</th>
      <th>with pgmajfault/s</th><th>without pgmajfault/s</th>
      <th>with PSI some%</th><th>without PSI some%</th></tr>
  {top10_html}
</tbody></table>
{png_block}
<h2>PSI (Memory Pressure) 분석</h2>
<p>PSI some avg10: 전체 task 중 메모리 부족으로 지연된 비율(%).</p>
<table class="wrapped"><tbody>
  <tr><th>cgroup</th><th>with_cgrd PSI some%</th>
      <th>without_cgrd PSI some%</th><th>배율</th></tr>
  {psi_rows}
</tbody></table>
<h2>주요 관찰</h2>
<table class="wrapped"><tbody>
  <tr><th>관찰 항목</th><th>평가</th><th>설명</th></tr>
  <tr><td>system.slice 메모리 감소</td><td>✅ 긍정적</td>
      <td>cgrd가 system.slice 전체를 효과적으로 reclaim</td></tr>
  <tr><td>sam.service 메모리 감소</td><td>✅ 긍정적</td>
      <td>SAM 프로세스 메모리 회수</td></tr>
  <tr><td>system.slice pgmajfault 급증</td><td>🔴 부정적</td>
      <td>swap에서 페이지 재로드 I/O 급증</td></tr>
  <tr><td>surface-manager PSI 상승</td><td>🔴 부정적</td>
      <td>surface-manager가 메모리 압박 강하게 받음</td></tr>
  <tr><td>전체 시스템 PSI 증가</td><td>⚠️ 주의</td>
      <td>메모리 pressure 지수 상승으로 체감 성능 영향 가능</td></tr>
</tbody></table>
{data_path_macro(session_id)}
"""


def build_syslog_body(session_id: str, report_dir: Path,
                      has_timeline: bool, has_stats: bool) -> str:
    parts = [info_panel("cgreclaim Syslog 타이밍 분석 보고서",
                        f"<p>Session: <code>{session_id}</code> | "
                        f"YouTube→Netflix 앱 전환 타이밍 분석</p>")]

    # stats_report.md from THIS session (not global splash_analysis.md)
    stats_md = report_dir / "stats_report.md"
    if stats_md.exists():
        parts.append(md_to_storage(stats_md.read_text()))
    else:
        parts.append("<p><em>stats_report.md 없음 — run_bench.sh -s 로 수집 필요</em></p>")

    if has_timeline:
        parts.append(img_macro("timeline_comparison.png",
                               "Netflix 런치 타임라인 비교 (with vs without cgrd)"))
    if has_stats:
        parts.append(img_macro("stats_comparison.png",
                               "런치 타이밍 통계 — min / avg / max (outlier 제외)"))

    parts.append(data_path_macro(session_id))
    return "\n".join(parts)


# ── main ──────────────────────────────────────────────────────────────────────

def get_latest_session(report_dir: Path) -> str:
    sessions = sorted([d.name for d in report_dir.iterdir()
                       if d.is_dir() and re.match(r"\d{8}_\d{6}", d.name)])
    if not sessions:
        raise FileNotFoundError(f"No sessions found in {report_dir}")
    return sessions[-1]


def main():
    parser = argparse.ArgumentParser(
        description="Upload cgreclaim analysis to Confluence (parent + 3 child pages)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--session", help="Session ID (default: latest)")
    parser.add_argument("--user", default=os.environ.get("CONFLUENCE_USER"))
    parser.add_argument("--password", default=os.environ.get("CONFLUENCE_PASS"))
    parser.add_argument("--parent-page-id",
                        default=DEFAULT_PARENT_PAGE_ID,
                        help=f"Parent page ID (default: {DEFAULT_PARENT_PAGE_ID})")
    parser.add_argument("--report-dir", default=str(DEFAULT_REPORT_DIR))
    parser.add_argument("--data-dir", default=str(DEFAULT_DATA_DIR))
    parser.add_argument("--dry-run", action="store_true",
                        help="Build page content but do not upload")
    args = parser.parse_args()

    if not args.user or not args.password:
        print("ERROR: --user / --password required (or set CONFLUENCE_USER/CONFLUENCE_PASS)")
        sys.exit(1)

    report_dir = Path(args.report_dir)
    data_dir_base = Path(args.data_dir)
    session_id = args.session or get_latest_session(report_dir)
    session_report_dir = report_dir / session_id
    # Support both: --data-dir tools/data (base) or --data-dir tools/data/SESSION (direct)
    if (data_dir_base / "with_cgrd").exists() or (data_dir_base / "without_cgrd").exists():
        session_data_dir = data_dir_base  # already points to session dir
    else:
        session_data_dir = data_dir_base / session_id

    print(f"Session    : {session_id}")
    print(f"Report dir : {session_report_dir}")
    print(f"Data dir   : {session_data_dir}")
    print(f"Parent ID  : {args.parent_page_id}")

    now_str = datetime.now().strftime("%Y-%m-%d %H:%M")
    date_str = datetime.now().strftime("%Y-%m-%d")

    # Detect available content
    has_timeline = (session_report_dir / "timeline_comparison.png").exists()
    has_stats    = (session_report_dir / "stats_comparison.png").exists()
    has_meminfo  = (session_report_dir / "meminfo_comparison.png").exists()
    has_cgroup   = (session_report_dir / "cgroup_comparison.png").exists()
    has_syslog   = has_timeline or has_stats
    has_memory   = has_meminfo or has_cgroup
    has_mem_data = session_data_dir.exists() and any(
        session_data_dir.glob("with_cgrd/run_*/meminfo_sampler/*.csv"))
    has_cg_data  = session_data_dir.exists() and any(
        session_data_dir.glob("with_cgrd/run_*/cgroup_sampler/*.csv"))

    if args.dry_run:
        print("[dry-run] Would create:")
        print(f"  Parent: 'cgreclaim 분석 {date_str} [{session_id}]'")
        if has_syslog:   print(f"  Child:  'Syslog 타이밍 분석 [{session_id}]'")
        if has_mem_data: print(f"  Child:  '메모리 분석 [{session_id}]'")
        if has_cg_data:  print(f"  Child:  'Cgroup 분석 [{session_id}]'")
        return

    s = requests.Session()
    s.auth = HTTPBasicAuth(args.user, args.password)

    display_name = check_auth(s)
    print(f"  ✓ Logged in as: {display_name}")
    space_key = get_space_key(s, args.parent_page_id)
    print(f"  ✓ Space key: {space_key}")

    # Step 1: create/update the date parent page FIRST (child pages go under it)
    print("\n[Parent] Creating/updating summary page...")
    parent_title = f"cgreclaim 분석 {date_str} [{session_id}]"
    r = s.get(f"{CONFLUENCE_BASE}/rest/api/content",
              params={"title": parent_title, "spaceKey": space_key,
                      "expand": "version"}, timeout=10)
    existing = r.json().get("results", [])
    if existing:
        date_page_id = existing[0]["id"]
        print(f"  (exists, id={date_page_id})")
    else:
        date_page_id = create_child_page(s, args.parent_page_id, space_key,
                                         parent_title, "<p>분석 문서 생성 중...</p>")

    # Step 2: create child pages under the date parent page
    child_pages: dict = {}  # title -> page_id

    # Syslog child page
    if has_syslog:
        print("\n[1/3] Creating Syslog 타이밍 분석 page...")
        syslog_title = f"Syslog 타이밍 분석 [{session_id}]"
        syslog_id = get_or_create_page(s, date_page_id, space_key,
                                       syslog_title, "")
        if has_timeline:
            upload_attachment(s, syslog_id, session_report_dir / "timeline_comparison.png")
        if has_stats:
            upload_attachment(s, syslog_id, session_report_dir / "stats_comparison.png")
        body = build_syslog_body(session_id, session_report_dir, has_timeline, has_stats)
        update_page_content(s, syslog_id, body)
        child_pages[syslog_title] = syslog_id

    # Meminfo child page
    if has_mem_data:
        print("\n[2/3] Creating 메모리 분석 page...")
        mem_title = f"메모리 분석 [{session_id}]"
        mem_id = get_or_create_page(s, date_page_id, space_key,
                                    mem_title, "")
        if has_meminfo:
            upload_attachment(s, mem_id, session_report_dir / "meminfo_comparison.png")
        body = build_meminfo_body(session_id, session_data_dir, session_report_dir, has_meminfo)
        update_page_content(s, mem_id, body)
        child_pages[mem_title] = mem_id

    # Cgroup child page
    if has_cg_data:
        print("\n[3/3] Creating Cgroup 분석 page...")
        cg_title = f"Cgroup 분석 [{session_id}]"
        cg_id = get_or_create_page(s, date_page_id, space_key,
                                   cg_title, "")
        if has_cgroup:
            upload_attachment(s, cg_id, session_report_dir / "cgroup_comparison.png")
        body = build_cgroup_body(session_id, session_data_dir, has_cgroup)
        update_page_content(s, cg_id, body)
        child_pages[cg_title] = cg_id

    # Step 3: update parent summary with links to child pages
    print("\n[Parent] Updating summary with child links...")
    summary_body = build_summary_body(session_id, has_syslog, has_memory,
                                      child_pages, now_str)
    update_page_content(s, date_page_id, summary_body)

    print(f"\n✅ Done!")
    print(f"  Parent : {CONFLUENCE_BASE}/pages/viewpage.action?pageId={date_page_id}")
    for title, pid in child_pages.items():
        print(f"  Child  : {CONFLUENCE_BASE}/pages/viewpage.action?pageId={pid} — {title}")


if __name__ == "__main__":
    main()

