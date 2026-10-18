#!/usr/bin/env python3
"""Regression checks for source-driven RHI reference generation.

Run with: python docs/dy_engine_docs/component/test_generate.py
All fixtures live in temporary directories; the checked-in reference is untouched.
"""

from __future__ import annotations

from copy import deepcopy
import hashlib
import html
from html.parser import HTMLParser
import importlib.util
import io
import json
from pathlib import Path
import posixpath
import re
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import patch
from urllib.parse import unquote, urlsplit


COMPONENT = Path(__file__).resolve().parent
sys.path.insert(0, str(COMPONENT))
SPEC = importlib.util.spec_from_file_location("rhi_component_generate_tests", COMPONENT / "generate.py")
generate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generate
SPEC.loader.exec_module(generate)
from parser import parse_header  # noqa: E402


class PageLinks(HTMLParser):
    def __init__(self, source):
        super().__init__(convert_charrefs=True)
        self.ids = set()
        self.duplicate_ids = set()
        self.hrefs = []
        self.document_root = None
        self.nav_groups = []
        self.nav_subgroups = []
        self.nav_urls = []
        self.anchor_texts = []
        self.anchor_parts = None
        self.feed(source)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            if attrs["id"] in self.ids:
                self.duplicate_ids.add(attrs["id"])
            self.ids.add(attrs["id"])
        if "href" in attrs:
            self.hrefs.append(attrs["href"])
        if "src" in attrs:
            self.hrefs.append(attrs["src"])
        if tag == "body":
            self.document_root = attrs.get("data-doc-root")
        if tag == "details" and "nav-group" in attrs.get("class", "").split():
            self.nav_groups.append(attrs)
        if tag == "details" and "nav-subgroup" in attrs.get("class", "").split():
            self.nav_subgroups.append(attrs)
        if tag == "a" and "nav-symbol" in attrs.get("class", "").split():
            self.nav_urls.append(attrs["href"])
        if tag == "a":
            self.anchor_parts = []

    def handle_endtag(self, tag):
        if tag == "a" and self.anchor_parts is not None:
            self.anchor_texts.append("".join(self.anchor_parts).strip())
            self.anchor_parts = None

    def handle_data(self, data):
        if self.anchor_parts is not None:
            self.anchor_parts.append(data)


class GenerateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="rhi-reference-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.headers = Path(self.temporary.name) / "headers"
        self.headers.mkdir()

    def write_header(self, name, source):
        path = self.headers / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source, encoding="utf-8")
        self.assertFalse(path.read_bytes().startswith(b"\xef\xbb\xbf"))
        return path

    def reference(self):
        return generate.Reference(headers=self.headers)

    def write_group_fixture(self):
        project = Path(self.temporary.name) / "project"
        public = project / "src" / "Public"
        for header, source in {
            "dyf/Renderer.h": "namespace dyf { struct Renderer { bool Render(); }; }\n",
            "dyf/Canvas.h": "namespace dyf { struct Canvas { void Clear(); }; }\n",
            "dyf/RHI/Device.h": "namespace dyf::RHI { struct Device { bool Ready(); }; }\n",
        }.items():
            path = public / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")
        return project, {
            "dyf": {"그리기": ["dyf::Canvas"], "렌더링": ["dyf::Renderer"]},
            "RHI": {"장치": ["dyf::RHI::Device"]},
        }

    def write_owner_fixture(self):
        project = Path(self.temporary.name) / "owned-project"
        public = project / "src" / "Public"
        public.mkdir(parents=True)
        (public / "Math.h").write_text(
            "namespace sample {\n"
            "struct Vec { float x; };\n"
            "struct Quat { float w; };\n"
            "struct VecExtra { float x; };\n"
            "enum class Flags { Ready = 1 };\n"
            "using Count = unsigned;\n"
            "union Bits { float value; unsigned raw; };\n"
            "/// 벡터 두 개의 내적입니다.\n"
            "float Dot(const Vec& left, const Vec& right);\n"
            "/// 쿼터니언 두 개의 내적입니다.\n"
            "float Dot(const Quat& left, const Quat& right);\n"
            "/// 벡터의 크기를 곱합니다.\n"
            "Vec operator*(const Vec& value, float scale);\n"
            "/// 쿼터니언의 크기를 곱합니다.\n"
            "Quat operator*(const Quat& value, float scale);\n"
            "void Reset(Vec& value);\n"
            "Flags operator|(Flags left, Flags right);\n"
            "Count NextCount(Count count);\n"
            "void ClearBits(Bits& bits);\n"
            "float Mix(const Vec& left, const Quat& right);\n"
            "float Length(const VecExtra& value);\n"
            "void Tick();\n"
            "inline int version = 1;\n"
            "}\n", encoding="utf-8")
        owners = {
            "Dot": ["Vec", "Quat"], "Math-operator*": ["Vec", "Quat"],
            "Reset": "Vec", "Math-operator|": "Flags", "NextCount": "Count", "ClearBits": "Bits",
        }
        return project, owners

    def write_catalog(self, project, name, values):
        (project / name).write_text(json.dumps(values, ensure_ascii=False), encoding="utf-8")

    def build(self):
        reference = self.reference()
        return reference, reference.build()

    def text(self, pages):
        return html.unescape("\n".join(pages.values()))

    def assert_local_links_resolve(self, pages):
        parsed_pages = {name: PageLinks(source) for name, source in pages.items() if name.endswith(".html")}
        for name, parsed in parsed_pages.items():
            self.assertFalse(parsed.duplicate_ids, f"Duplicate anchors in {name}: {parsed.duplicate_ids}")
            for href in parsed.hrefs:
                self.assert_local_target(name, href, pages, parsed_pages)
        search_name = "component/assets/search-index.js"
        self.assertIn(search_name, pages)
        assignment = re.search(r"=\s*(\[[\s\S]*\])\s*;?\s*$", pages[search_name])
        self.assertIsNotNone(assignment, "Search index must contain a JSON array assignment")
        entries = json.loads(assignment.group(1))
        self.assertTrue(entries)
        # docs.js resolves each search URL against body[data-doc-root].
        for name, parsed in parsed_pages.items():
            self.assertIsNotNone(parsed.document_root, f"Search root missing in {name}")
            document_root = posixpath.normpath(posixpath.join(posixpath.dirname(name), parsed.document_root))
            self.assertEqual(document_root, ".", f"Search root must resolve to docs/rhi: {name}")
            for entry in entries:
                self.assert_local_target(posixpath.join(document_root, "index.html"), entry["url"], pages, parsed_pages, required=True)

    def assert_local_target(self, origin, href, pages, parsed_pages, required=False):
        parts = urlsplit(href)
        if parts.scheme or parts.netloc:
            return
        path = unquote(parts.path)
        if not path:
            target = origin
        else:
            target = posixpath.normpath(posixpath.join(posixpath.dirname(origin), path))
        if target not in pages and not target.endswith(".html") and not required:
            self.assertTrue((COMPONENT.parent / target).is_file(), f"Missing static asset: {origin} -> {href}")
            return
        self.assertIn(target, pages, f"Missing target: {origin} -> {href}")
        if parts.fragment:
            self.assertIn(target, parsed_pages, f"Anchor target is not HTML: {origin} -> {href}")
            self.assertTrue(unquote(parts.fragment) in parsed_pages[target].ids, f"Missing anchor: {origin} -> {href}")

    def test_new_namespace_headers_and_members_need_no_reference_json(self):
        self.write_header("Device.h", "namespace another_project::RHI { struct Device { int value = 4; }; }\n")
        reference, pages = self.build()
        self.assertEqual({d.name for d in reference.symbols.values()}, {"Device"})
        self.assertIn("another_project::RHI", self.text(pages))
        self.assertIn("int value = 4;", self.text(pages))

        self.write_header("Device.h", "namespace another_project::RHI { struct Device { int value = 4; bool Ready() const; }; }\n")
        self.write_header("nested/NewFeature.h", "namespace another_project::RHI { struct AddedFeature { unsigned count = 7; }; }\n")
        reference, pages = self.build()
        self.assertEqual({d.name for d in reference.symbols.values()}, {"Device", "AddedFeature"})
        self.assertIn("nested/NewFeature.h", reference.sources)
        self.assertIn("Ready() const", self.text(pages))
        self.assertIn("unsigned count = 7", self.text(pages))
        self.assert_local_links_resolve(pages)

    def test_comments_and_enum_source_lines_follow_the_header(self):
        source = (
            "#pragma once\n"
            "namespace renamed::RHI\n"
            "{\n"
            "    /// 작업 진행 상태입니다.\n"
            "    enum class Status\n"
            "    {\n"
            "        /// 준비된 상태입니다.\n"
            "        Ready = 3,\n"
            "        // 완료된 상태입니다.\n"
            "        Done,\n"
            "    };\n"
            "}\n"
        )
        path = self.write_header("Status.h", source)
        declarations = parse_header(path, source)
        status = next(d for d in declarations if d.name == "Status")
        self.assertEqual(status.namespace, "renamed::RHI")
        self.assertEqual(status.line, 5)
        self.assertIn("작업 진행 상태", status.comment)
        members = {m.name: m for m in status.members}
        self.assertEqual(members["Ready"].line, 8)
        self.assertEqual(members["Done"].line, 10)
        self.assertIn("준비된 상태", members["Ready"].comment)
        self.assertIn("완료된 상태", members["Done"].comment)
        _, pages = self.build()
        self.assertIn("작업 진행 상태", self.text(pages))
        self.assertIn("준비된 상태", self.text(pages))
        self.assert_local_links_resolve(pages)

    def test_signature_changes_are_visible_without_editing_prose(self):
        path = self.write_header("Worker.h", "namespace sample::RHI { class Worker { public: int Run(int count); }; }\n")
        _, original_pages = self.build()
        original_manifest = json.loads(original_pages["component/manifest.json"])
        path.write_text("namespace sample::RHI { class Worker { public: bool Run(float seconds, unsigned flags); }; }\n", encoding="utf-8")
        reference, pages = self.build()
        worker = next(d for d in reference.symbols.values() if d.name == "Worker")
        self.assertEqual(worker.members[0].syntax, "bool Run(float seconds, unsigned flags);")
        self.assertIn("bool Run(float seconds, unsigned flags);", self.text(pages))
        self.assertNotIn("int Run(int count);", self.text(pages))
        self.assertNotEqual(original_manifest["headers"], json.loads(pages["component/manifest.json"])["headers"])

    def test_header_move_updates_paths_and_all_document_links(self):
        path = self.write_header("Original.h", "namespace project::RHI { struct Resource { int count = 2; }; }\n")
        _, original_pages = self.build()
        source = path.read_text(encoding="utf-8")
        path.unlink()
        self.write_header("nested/deeper/Moved.h", source)
        reference, pages = self.build()
        self.assertEqual(set(reference.sources), {"nested/deeper/Moved.h"})
        self.assertNotIn("Original.h", self.text(pages))
        self.assertIn("nested/deeper/Moved.h", self.text(pages))
        self.assertNotEqual(set(original_pages), set(pages))
        self.assert_local_links_resolve(pages)

    def test_equal_header_basenames_remain_distinct(self):
        self.write_header("first/Types.h", "namespace project::RHI { struct First { int value; }; }\n")
        self.write_header("second/Types.h", "namespace project::RHI { struct Second { int value; }; }\n")
        reference, pages = self.build()
        self.assertEqual(set(reference.sources), {"first/Types.h", "second/Types.h"})
        self.assertEqual({d.header for d in reference.symbols.values()}, set(reference.sources))
        self.assert_local_links_resolve(pages)

    def test_empty_header_alongside_api_keeps_a_valid_source_link(self):
        self.write_header("Api.h", "namespace application { struct Api { int value; }; }\n")
        self.write_header("Empty.h", "")
        reference, pages = self.build()
        self.assertEqual(set(reference.sources), {"Api.h", "Empty.h"})
        self.assertEqual(reference.sources["Empty.h"], "")
        self.assertIn(reference.header_urls["Empty.h"], pages)
        target = urlsplit(reference.source_link("Empty.h"))
        self.assertIn(target.fragment, PageLinks(pages[target.path]).ids)
        self.assert_local_links_resolve(pages)

    def test_non_rhi_namespaces_and_global_apis_are_included(self):
        self.write_header("Globals.hpp", (
            "/// 전역 호출 횟수입니다.\n"
            "inline int globalCount = 2;\n"
            "union Bits { float value; unsigned raw; };\n"
            "void Tick();\n"
            "namespace application::Platform { class Window { public: void Open(); }; }\n"
            "namespace application { struct Camera { float depth; }; }\n"
        ))
        reference, pages = self.build()
        declarations = {(decl.namespace, decl.name, decl.kind) for decl in reference.symbols.values()}
        self.assertEqual(declarations, {
            ("", "globalCount", "variable"), ("", "Bits", "union"), ("", "Tick", "function"),
            ("application::Platform", "Window", "class"), ("application", "Camera", "struct"),
        })
        self.assertFalse(reference.diagnostics)
        self.assertIn("전역 호출 횟수입니다.", self.text(pages))
        self.assert_local_links_resolve(pages)

    def test_public_and_internal_definitions_keep_separate_pages_and_signatures(self):
        signatures = {"Public/RHI/Device.h": "bool Submit(int count);", "RHI/Device.h": "void Submit(float delay);"}
        for header, signature in signatures.items():
            self.write_header(header, f"namespace engine::RHI {{ class Device {{ public: {signature} }}; }}\n")
        reference, pages = self.build()
        devices = [(key, decl) for key, decl in reference.symbols.items() if decl.namespace == "engine::RHI" and decl.name == "Device"]
        self.assertEqual(len(devices), 2)
        self.assertEqual(len({reference.url(key) for key, _ in devices}), 2)
        home_links = PageLinks(pages["index.html"])
        index_links = PageLinks(pages["component/api-index.html"])
        for key, device in devices:
            own_signature = signatures[device.header]
            other_signature = next(signature for header, signature in signatures.items() if header != device.header)
            standalone = html.unescape(pages[reference.url(key)])
            self.assertIn(own_signature, standalone)
            self.assertNotIn(other_signature, standalone)
            self.assertIn(reference.url(key), home_links.nav_urls)
            self.assertIn(posixpath.basename(reference.url(key)), index_links.hrefs)
            self.assertNotIn(own_signature, html.unescape(pages["index.html"]))
        self.assert_local_links_resolve(pages)

    def test_conditional_macro_variants_retain_both_definitions(self):
        self.write_header("Feature.h", (
            "#pragma once\n"
            "#if defined(FEATURE)\n"
            "#define API_LIMIT 8\n"
            "#define API_SCALE(value) ((value) * 8)\n"
            "#else\n"
            "#define API_LIMIT 4\n"
            "#define API_SCALE(value) ((value) * 4)\n"
            "#endif\n"
            "namespace application { struct Feature { int enabled; }; }\n"
        ))
        reference, pages = self.build()
        for name in ("API_SCALE", "API_LIMIT"):
            with self.subTest(macro=name):
                key, macro = next((key, decl) for key, decl in reference.symbols.items() if decl.name == name)
                self.assertEqual(macro.kind, "macro")
                definitions = reference.overloads[key]
                self.assertEqual(len(definitions), 2)
                self.assertEqual(len({declaration.condition for declaration in definitions}), 2)
                rendered = html.unescape(pages[reference.url(key)])
                self.assertIn("조건:", rendered)
                for declaration in definitions:
                    self.assertIn("FEATURE", declaration.condition)
                    self.assertIn(declaration.condition, rendered)
                    self.assertIn(declaration.syntax, rendered)
        self.assert_local_links_resolve(pages)

    def test_nested_macro_branches_can_repeat_the_same_definition(self):
        self.write_header("Profiler.h", (
            "#if defined(PROFILER)\n"
            "#if defined(RAW_VALUES)\n"
            "#define PROFILE_VALUE(value) Record(value)\n"
            "#else\n"
            "#define PROFILE_VALUE(value) ((void)(value))\n"
            "#endif\n"
            "#else\n"
            "#define PROFILE_VALUE(value) ((void)(value))\n"
            "#endif\n"
        ))
        reference, pages = self.build()
        key = next(key for key, decl in reference.symbols.items() if decl.name == "PROFILE_VALUE")
        definitions = reference.overloads[key]
        self.assertEqual(len(definitions), 3)
        self.assertEqual(len({decl.syntax for decl in definitions}), 2)
        self.assertEqual(len({decl.condition for decl in definitions}), 3)
        rendered = html.unescape(pages[reference.url(key)])
        for declaration in definitions:
            self.assertIn(declaration.condition, rendered)
        self.assert_local_links_resolve(pages)

    def test_unexpanded_declarations_keep_diagnostics_and_source_text(self):
        self.write_header("Generated.h", (
            "#define DECLARE_WIDGET(name) struct name { int value; };\n"
            "DECLARE_WIDGET(HiddenWidget)\n"
            "struct Known { int value; };\n"
        ))
        reference, pages = self.build()
        self.assertIn("Known", {declaration.name for declaration in reference.symbols.values()})
        diagnosis = next(entry for entry in reference.diagnostics if "DECLARE_WIDGET(HiddenWidget)" in entry["syntax"])
        self.assertEqual(diagnosis["header"], "Generated.h")
        self.assertEqual(diagnosis["line"], 2)
        self.assertTrue(diagnosis["reason"].strip())
        self.assertIn(diagnosis, json.loads(pages["component/manifest.json"])["diagnostics"])
        self.assertNotIn("DECLARE_WIDGET(HiddenWidget)", html.unescape(pages["index.html"]))
        self.assertIn("DECLARE_WIDGET(HiddenWidget)", html.unescape(pages[reference.header_urls["Generated.h"]]))
        self.assert_local_links_resolve(pages)

    def test_empty_or_non_api_headers_fail_explicitly(self):
        for source in (None, "#pragma once\n// Public declarations have moved elsewhere.\n"):
            with self.subTest(source=source):
                if source is not None:
                    self.write_header("Empty.h", source)
                with self.assertRaises(ValueError) as caught:
                    self.build()
                self.assertTrue(str(caught.exception).strip(), "The error should explain why no API was found")

    def test_free_function_overloads_keep_every_signature(self):
        self.write_header("Upload.h", "namespace project::RHI { void Upload(int count); void Upload(float count); }\n")
        reference, pages = self.build()
        keys = [key for key, declaration in reference.symbols.items() if declaration.name == "Upload"]
        self.assertEqual(len(keys), 1)
        self.assertEqual(len(reference.overloads[keys[0]]), 2)
        rendered = self.text(pages)
        self.assertIn("void Upload(int count);", rendered)
        self.assertIn("void Upload(float count);", rendered)
        self.assert_local_links_resolve(pages)

    def test_friend_function_keeps_class_syntax_without_a_member_search_entry(self):
        self.write_header("Asset.h", (
            "namespace application {\n"
            "class Asset {\n"
            "public:\n"
            "    friend bool Inspect(const Asset& asset);\n"
            "};\n"
            "bool Inspect(const Asset& asset);\n"
            "}\n"
        ))
        reference, pages = self.build()
        class_key, asset = next((key, decl) for key, decl in reference.symbols.items() if decl.name == "Asset")
        function_key, function = next((key, decl) for key, decl in reference.symbols.items() if decl.name == "Inspect")
        self.assertEqual(function.kind, "function")
        self.assertIn("friend bool Inspect(const Asset& asset);", asset.syntax)
        self.assertIn("friend bool Inspect(const Asset& asset);", html.unescape(pages[reference.url(class_key)]))
        assignment = re.search(r"=\s*(\[[\s\S]*\])\s*;?\s*$", pages["component/assets/search-index.js"])
        entries = json.loads(assignment.group(1))
        self.assertIn(function_key, {entry["title"] for entry in entries})
        self.assertNotIn(class_key + "::Inspect", {entry["title"] for entry in entries})
        self.assert_local_links_resolve(pages)

    def test_colliding_symbol_slugs_fail_instead_of_overwriting_pages(self):
        self.write_header("Types.h", "namespace project::RHI { struct Foo {}; struct foo {}; }\n")
        with self.assertRaises(ValueError) as caught:
            self.build()
        self.assertTrue(str(caught.exception).strip())

    def test_nested_public_types_keep_comments_search_entries_and_anchors(self):
        self.write_header("Container.h", (
            "namespace project::RHI {\n"
            "class Container {\n"
            "public:\n"
            "    /// 토큰 형식의 설명입니다.\n"
            "    using Token = unsigned;\n"
            "    /// 중첩 설정의 설명입니다.\n"
            "    struct Settings { int limit = 3; };\n"
            "    /// 동작 모드의 설명입니다.\n"
            "    enum class Mode { Fast = 1, Safe = 2 };\n"
            "};\n"
            "}\n"
        ))
        reference, pages = self.build()
        key, container = next((key, decl) for key, decl in reference.symbols.items() if decl.name == "Container")
        members = {member.name: member for member in container.members}
        symbol_page = pages[reference.url(key)]
        for name, kind, comment in (("Token", "alias", "토큰 형식의 설명입니다."),
                                    ("Settings", "struct", "중첩 설정의 설명입니다."),
                                    ("Mode", "enum", "동작 모드의 설명입니다.")):
            with self.subTest(member=name):
                self.assertEqual(members[name].kind, kind)
                self.assertEqual(members[name].comment, comment)
                self.assertIn(comment, html.unescape(symbol_page))
                self.assertIn("Container::" + name, pages["component/assets/search-index.js"])
        self.assert_local_links_resolve(pages)

    def test_anonymous_aggregate_fields_are_searchable_with_conditional_variants(self):
        self.write_header("Vector.h", (
            "namespace math {\n"
            "struct SimdVector { float lanes[4]; };\n"
            "struct ScalarVector { float lanes[4]; };\n"
            "struct Outer {\n"
            "    union {\n"
            "        struct { float x, y; };\n"
            "#if defined(USE_SIMD)\n"
            "        SimdVector simd;\n"
            "#else\n"
            "        ScalarVector simd;\n"
            "#endif\n"
            "    };\n"
            "};\n"
            "}\n"
        ))
        reference, pages = self.build()
        key, outer = next((key, decl) for key, decl in reference.symbols.items() if decl.name == "Outer")
        members = reference.members[key]
        self.assertEqual({member.name for member in members}, {"x", "y", "simd"})
        variants = [member for member in members if member.name == "simd"]
        self.assertEqual(len(variants), 2)
        self.assertEqual(len({member.condition for member in variants}), 2)
        self.assertRegex(outer.syntax, r"#if[^\n]*USE_SIMD")
        self.assertIn("#endif", outer.syntax)
        standalone = pages[reference.url(key)]
        self.assertEqual(standalone.count('id="member-simd"'), 1)
        for variant in variants:
            self.assertIn(variant.syntax, html.unescape(standalone))
            self.assertIn(variant.condition, html.unescape(standalone))
            self.assertIn(variant.syntax, outer.syntax)
        assignment = re.search(r"=\s*(\[[\s\S]*\])\s*;?\s*$", pages["component/assets/search-index.js"])
        titles = [entry["title"] for entry in json.loads(assignment.group(1))]
        for name in ("x", "y", "simd"):
            self.assertEqual(titles.count(f"{key}::{name}"), 1)
        self.assertFalse(any(re.search(r"anonymous-(?:struct|union)-\d+", title) for title in titles))
        self.assert_local_links_resolve(pages)

    def test_comments_are_html_escaped_and_literal_whitespace_is_preserved(self):
        comment = '<script>alert("x")</script> & \'quoted\''
        self.write_header("Label.h", (
            "namespace project::RHI {\n"
            f"/// {comment}\n"
            "struct Label {\n"
            f"    /// {comment}\n"
            '    const char* name = "a  b";\n'
            "};\n"
            "}\n"
        ))
        reference, pages = self.build()
        key, label = next((key, decl) for key, decl in reference.symbols.items() if decl.name == "Label")
        self.assertIn('"a  b"', label.members[0].syntax)
        symbol_page = pages[reference.url(key)]
        self.assertIn('"a  b"', html.unescape(symbol_page))
        self.assertIn(html.escape(comment, quote=True), symbol_page)
        for name, source in pages.items():
            if name.endswith(".html"):
                self.assertNotIn('<script>alert("x")</script>', source, name)
        self.assert_local_links_resolve(pages)

    def test_attributes_constructors_noexcept_and_conversion_operators(self):
        self.write_header("Guard.h", (
            "namespace project::RHI {\n"
            "class Guard {\n"
            "public:\n"
            '    [[deprecated("Use factory")]] explicit Guard(int value) noexcept : value_(value) {}\n'
            "    [[nodiscard]] bool Ready() const noexcept(true);\n"
            "    explicit operator bool() const noexcept { return value_ != 0; }\n"
            "private:\n"
            "    int value_;\n"
            "};\n"
            "}\n"
        ))
        reference, pages = self.build()
        guard = next(decl for decl in reference.symbols.values() if decl.name == "Guard")
        members = {member.name.replace(" ", ""): member for member in guard.members}
        self.assertEqual(set(members), {"Guard", "Ready", "operatorbool"})
        self.assertIn("noexcept", members["Guard"].syntax)
        self.assertNotIn("value_(value)", members["Guard"].syntax)
        self.assertIn("[[nodiscard]]", members["Ready"].syntax)
        self.assertIn("noexcept(true)", members["Ready"].syntax)
        self.assertNotIn("return", members["operatorbool"].syntax)
        self.assert_local_links_resolve(pages)

    def test_generated_files_stay_under_component_except_the_landing_page(self):
        self.write_header("Simple.h", "namespace project::RHI { struct Simple { int size = 1; }; }\n")
        _, pages = self.build()
        self.assertIn("index.html", pages)
        self.assertIn("component/manifest.json", pages)
        self.assertIn("component/assets/search-index.js", pages)
        for name, content in pages.items():
            self.assertTrue(name == "index.html" or name.startswith("component/"), name)
            self.assertNotIn("\\", name)
            self.assertNotIn("..", Path(name).parts)
            self.assertFalse(content.startswith("\ufeff"), name)
        self.assert_local_links_resolve(pages)

    def test_home_links_modules_and_api_navigation_uses_detail_pages(self):
        expected = {}
        for header, prefix, count in (("Texture.h", "Texture", 7), ("Extra.h", "Extra", 1), ("nested/Buffer.h", "Buffer", 2)):
            declarations = []
            for number in range(count):
                name = f"{prefix}{number}"
                comment = f"{name}의 상세 설명입니다."
                member_comment = f"{name}의 공유 이름 멤버 설명입니다."
                declaration = (
                    f"/// {comment}\n"
                    f"struct {name} {{\n"
                    f"    /// {member_comment}\n"
                    f"    int value = {number};\n"
                    f"    /// {name}의 실행 설명입니다.\n"
                    "    bool Run(int count);\n"
                    "};\n"
                )
                declarations.append(declaration)
                expected[name] = (comment, member_comment, f"int value = {number};")
            self.write_header(header, "namespace project::RHI {\n" + "\n".join(declarations) + "}\n")

        reference, pages = self.build()
        home = pages["index.html"]
        home_links = PageLinks(home)
        api_index = PageLinks(pages["component/api-index.html"])
        self.assertNotIn('class="reference-api"', home)
        self.assertNotIn('class="reference-module"', home)
        self.assertNotIn("bool Run(int count);", html.unescape(home))
        self.assertFalse(any(anchor.startswith("api-") for anchor in home_links.ids))
        self.assertNotIn("전체 API", home)
        self.assertNotIn('class="top-nav"', home)
        self.assertIn("Source API Reference", home)
        self.assertNotIn("Source API Reference", home_links.anchor_texts)
        groups = home_links.nav_groups
        self.assertEqual({group["data-group"] for group in groups}, {"기본", "nested"})
        self.assertNotIn("component/getting-started.html", pages)
        self.assertNotIn("component/headers.html", pages)
        self.assertTrue(all("open" not in group for group in groups))
        self.assertEqual(set(reference.api_groups), {"기본", "nested"})
        for module, values in reference.by_module.items():
            self.assertEqual(set(reference.api_groups[module]), {"API"})
            self.assertEqual(set(reference.api_groups[module]["API"]), {key for key, _ in values})
        self.assertEqual({(group["data-module"], group["data-category"]) for group in home_links.nav_subgroups},
                         {("기본", "API"), ("nested", "API")})
        self.assertTrue(all("open" not in group for group in home_links.nav_subgroups))
        self.assertEqual(set(home_links.nav_urls), set(reference.symbol_urls.values()))
        for key, declaration in reference.symbols.items():
            with self.subTest(api=declaration.name):
                url = reference.url(key)
                self.assertRegex(url, r"^component/api-[^/]+\.html$")
                self.assertIn(posixpath.basename(url), api_index.hrefs)
                detail = html.unescape(pages[url])
                for expected_text in expected[declaration.name]:
                    self.assertIn(expected_text, detail)
                self.assertIn("bool Run(int count);", detail)
                self.assertNotIn(expected[declaration.name][1], html.unescape(home))
                detail_links = PageLinks(pages[url])
                self.assertIn("member-value", detail_links.ids)
                self.assertIn("member-run", detail_links.ids)
                opened_groups = [group["data-group"] for group in detail_links.nav_groups if "open" in group]
                expected_module = "nested" if declaration.name.startswith("Buffer") else "기본"
                self.assertEqual(opened_groups, [expected_module])
                self.assertEqual([(group["data-module"], group["data-category"])
                                  for group in detail_links.nav_subgroups if "open" in group],
                                 [(expected_module, "API")])

        for module in ("기본", "nested"):
            anchor = reference.module_anchor(module)
            self.assertIn("component/api-index.html#" + anchor, home_links.hrefs)
            self.assertIn(anchor, api_index.ids)
            self.assertNotIn(anchor, home_links.ids)
        assignment = re.search(r"=\s*(\[[\s\S]*\])\s*;?\s*$", pages["component/assets/search-index.js"])
        entries = json.loads(assignment.group(1))
        by_title = {entry["title"]: entry for entry in entries}
        for key in reference.symbols:
            self.assertEqual(by_title[key]["url"], reference.url(key))
            for member in ("value", "Run"):
                self.assertEqual(by_title[f"{key}::{member}"]["url"], reference.url(key) + "#" + generate.member_anchor(member))
        for entry in entries:
            self.assertIn(urlsplit(entry["url"]).path, reference.symbol_urls.values())
        self.assert_local_links_resolve(pages)

    def test_source_directory_option_accepts_repository_relative_and_absolute_paths(self):
        self.write_header("Chosen.h", "namespace chosen { struct Selected { bool available; }; }\n")
        output = Path(self.temporary.name) / "output"
        with patch.object(generate, "ROOT", Path(self.temporary.name)), patch.object(generate, "OUTPUT", output):
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.assertEqual(generate.main(["--source-dir", "headers"]), 0)
                self.assertEqual(generate.main(["--source-dir", str(self.headers), "--check"]), 0)
            manifest = json.loads((output / "component/manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(set(manifest["headers"]), {"Chosen.h"})
            self.assertEqual(manifest["source_root"], "headers")
            self.assertIn("Selected", (output / "index.html").read_text(encoding="utf-8"))

    def test_default_source_contains_only_current_public_apis(self):
        reference = generate.Reference()
        self.assertEqual(reference.headers, (generate.ROOT / "src" / "Public").resolve())
        declared = {(decl.header, decl.namespace, decl.name) for decl in reference.symbols.values()}
        for expected in (
            ("dyf/Renderer.h", "dyf", "Renderer"),
            ("dyf/Math/Math.h", "dyf::Math", "float3"),
            ("dyf/Platform/Window.h", "dyf::Platform", "Window"),
            ("dyf/RHI/IDevice.h", "dyf::RHI", "IDevice"),
            ("dyf/RHI/RenderGraph.h", "dyf::RHI", "RenderGraph"),
        ):
            with self.subTest(api=expected):
                self.assertIn(expected, declared)
        self.assertFalse(any("Backends" in header or "Private" in header for header, _, _ in declared))
        self.assertEqual(list(reference.by_module), ["dyf", "RHI", "Math", "Platform", "확장: Model"])
        for module, name in (("dyf", "Renderer"), ("RHI", "RenderGraph"), ("Math", "float3"),
                             ("Platform", "DY_PROFILE_FRAME_MARK"), ("확장: Model", "ModelAsset")):
            self.assertIn(name, [decl.name for _, decl in reference.by_module[module]])
        self.assertIn("api_groups.json", reference.authored_hashes)
        classified = [key for categories in reference.api_groups.values()
                      for keys in categories.values() for key in keys]
        self.assertCountEqual(classified, reference.api_pages)

    def test_related_functions_render_on_owner_pages_with_per_overload_search_and_links(self):
        project, owners = self.write_owner_fixture()
        groups = {"기본": {"벡터": ["Vec", "VecExtra"], "회전": ["Quat"],
                           "기타": ["Flags", "Count", "Bits", "Mix", "Length", "Tick", "version"]}}
        self.write_catalog(project, "api_owners.json", owners)
        self.write_catalog(project, "api_groups.json", groups)
        with patch.object(generate, "ROOT", project), patch.object(generate, "HERE", project):
            reference = generate.Reference()
            pages = reference.build()
        self.assertEqual(reference.api_groups, groups)
        self.assertEqual(reference.function_owners["Dot"], ["Vec", "Quat"])
        self.assertEqual(set(reference.api_pages), set(reference.symbols) - owners.keys())
        self.assertIn("api_owners.json", reference.authored_hashes)
        manifest = json.loads(pages["component/manifest.json"])
        self.assertEqual(manifest["authored_files"]["api_owners.json"], reference.authored_hashes["api_owners.json"])
        home_links = PageLinks(pages["index.html"])
        self.assertCountEqual(home_links.nav_urls, [reference.url(key) for key in reference.api_pages])
        index_links = PageLinks(pages["component/api-index.html"])
        assignment = re.search(r"=\s*(\[[\s\S]*\])\s*;?\s*$", pages["component/assets/search-index.js"])
        entries = json.loads(assignment.group(1))
        for function, attached_owners in owners.items():
            attached_owners = [attached_owners] if isinstance(attached_owners, str) else attached_owners
            old_path = "component/api-" + generate.slug(function) + ".html"
            self.assertNotIn(old_path, pages)
            self.assertEqual(reference.url(function), reference.related_url(attached_owners[0], function))
            self.assertNotIn(posixpath.basename(old_path), index_links.hrefs)
            function_entries = [entry for entry in entries
                                if entry["title"] in [f"sample::{reference.symbols[function].name} ({owner})"
                                                     for owner in attached_owners]]
            self.assertCountEqual([entry["url"] for entry in function_entries],
                                  [reference.related_url(owner, function) for owner in attached_owners])
            for owner in attached_owners:
                with self.subTest(function=function, owner=owner):
                    rendered = html.unescape(pages[reference.url(owner)])
                    links = PageLinks(pages[reference.url(owner)])
                    self.assertIn(generate.related_anchor(function), links.ids)
                    self.assertEqual(reference.related_url(owner, function),
                                     reference.url(owner) + "#" + generate.related_anchor(function))
                    for declaration in reference.related_functions[owner][function]:
                        self.assertIn(declaration.syntax, rendered)
                        if declaration.comment:
                            self.assertIn(declaration.comment, rendered)
                        source = reference.source_link(declaration.header, declaration.line)
                        self.assertIn(posixpath.basename(source), links.hrefs)
                    for other_owner in set(attached_owners) - {owner}:
                        for declaration in reference.related_functions[other_owner][function]:
                            self.assertNotIn(declaration.syntax, rendered)
                            if declaration.comment:
                                self.assertNotIn(declaration.comment, rendered)
                    self.assertNotIn(f"{owner}::{reference.symbols[function].name}", rendered)
                    self.assertIn("sample::" + reference.symbols[function].name, rendered)
                    category = next(category for category, keys in groups["기본"].items() if owner in keys)
                    self.assertEqual([(group["data-module"], group["data-category"])
                                      for group in links.nav_subgroups if "open" in group], [("기본", category)])
        self.assertIn(reference.url("Tick"), pages)
        self.assert_local_links_resolve(pages)

    def test_api_owners_reject_invalid_targets_and_ambiguous_or_missing_overloads(self):
        project, _ = self.write_owner_fixture()
        invalid = {
            "unknown function": {"Missing": "Vec"},
            "non-function": {"Vec": "Quat"},
            "unknown owner": {"Reset": "Missing"},
            "function owner": {"Reset": "Tick"},
            "variable owner": {"Reset": "version"},
            "duplicate owner": {"Dot": ["Vec", "Vec", "Quat"]},
            "empty owners": {"Dot": []},
            "unclassified overload": {"Dot": ["Vec"]},
            "ambiguous overload": {"Mix": ["Vec", "Quat"]},
            "unused owner": {"Reset": ["Vec", "Quat"]},
            "owner matching uses type tokens": {"Length": ["Vec"]},
        }
        with patch.object(generate, "ROOT", project), patch.object(generate, "HERE", project):
            for name, catalog in invalid.items():
                with self.subTest(invalid=name):
                    self.write_catalog(project, "api_owners.json", catalog)
                    with self.assertRaises(ValueError) as caught:
                        generate.Reference()
                    self.assertTrue(str(caught.exception).strip())

    def test_attached_functions_cannot_remain_in_page_groups(self):
        project, owners = self.write_owner_fixture()
        self.write_catalog(project, "api_owners.json", owners)
        with patch.object(generate, "ROOT", project), patch.object(generate, "HERE", project):
            reference = generate.Reference()
            groups = deepcopy(reference.api_groups)
            groups["기본"]["API"].append("Dot")
            self.write_catalog(project, "api_groups.json", groups)
            with self.assertRaises(ValueError) as caught:
                generate.Reference()
            self.assertTrue(str(caught.exception).strip())

    def test_api_owners_fallback_and_custom_headers_do_not_load_project_catalog(self):
        project, _ = self.write_owner_fixture()
        self.write_header("Thing.h", "namespace fixture { struct Thing {}; void Update(Thing& value); }\n")
        with patch.object(generate, "ROOT", project), patch.object(generate, "HERE", project):
            reference = generate.Reference()
            self.assertFalse(reference.function_owners)
            self.assertFalse(reference.related_functions)
            self.assertEqual(reference.api_pages, reference.symbols)
            self.assertIn(reference.url("Dot"), reference.build())
            self.assertNotIn("api_owners.json", reference.authored_hashes)
            (project / "api_owners.json").write_text("invalid JSON", encoding="utf-8")
            custom = generate.Reference(headers=self.headers)
            self.assertFalse(custom.function_owners)
            self.assertEqual(custom.api_pages, custom.symbols)
            self.assertNotIn("api_owners.json", custom.authored_hashes)
            pages = custom.build()
            self.assertIn(custom.url("Update"), pages)
            self.assert_local_links_resolve(pages)

    def test_attaching_functions_removes_their_old_generated_pages(self):
        project, owners = self.write_owner_fixture()
        output = Path(self.temporary.name) / "owned-output"
        (project / "assets").mkdir()
        for name in ("docs.css", "docs.js", "theme.js"):
            (project / "assets" / name).write_text((COMPONENT / "assets" / name).read_text(encoding="utf-8"), encoding="utf-8")
        with patch.object(generate, "ROOT", project), patch.object(generate, "HERE", project), patch.object(generate, "OUTPUT", output):
            def run(*args):
                with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    return generate.main(list(args))

            def assert_search_version():
                search_source = (output / "component/assets/search-index.js").read_bytes()
                version = hashlib.sha256(search_source).hexdigest()[:12]
                for path in output.rglob("*.html"):
                    links = PageLinks(path.read_text(encoding="utf-8"))
                    scripts = [urlsplit(href) for href in links.hrefs
                               if urlsplit(href).path.endswith("/search-index.js")]
                    self.assertEqual(len(scripts), 1, path)
                    self.assertEqual(scripts[0].query, "v=" + version, path)
                return version

            original = generate.Reference()
            old_paths = [output / original.url(key) for key in owners]
            self.assertEqual(run(), 0)
            self.assertTrue(all(path.is_file() for path in old_paths))
            original_search_version = assert_search_version()
            self.write_catalog(project, "api_owners.json", owners)
            self.assertEqual(run("--check"), 1)
            self.assertTrue(all(path.is_file() for path in old_paths))
            self.assertEqual(assert_search_version(), original_search_version)
            self.assertEqual(run(), 0)
            self.assertTrue(all(not path.exists() for path in old_paths))
            self.assertTrue((output / original.url("Vec")).is_file())
            self.assertNotEqual(assert_search_version(), original_search_version)
            self.assertEqual(run("--check"), 0)

    def test_authored_api_groups_expand_only_the_selected_category(self):
        project, catalog = self.write_group_fixture()
        path = project / "api_groups.json"
        path.write_text(json.dumps(catalog, ensure_ascii=False), encoding="utf-8")
        with patch.object(generate, "ROOT", project), patch.object(generate, "HERE", project):
            reference = generate.Reference()
            pages = reference.build()
        self.assertEqual(reference.api_groups, catalog)
        self.assertIn("api_groups.json", reference.authored_hashes)
        manifest = json.loads(pages["component/manifest.json"])
        self.assertEqual(manifest["authored_files"]["api_groups.json"], reference.authored_hashes["api_groups.json"])
        expected_categories = [(module, category) for module, categories in catalog.items() for category in categories]
        home_links = PageLinks(pages["index.html"])
        self.assertEqual([(group["data-module"], group["data-category"]) for group in home_links.nav_subgroups],
                         expected_categories)
        self.assertTrue(all("open" not in group for group in home_links.nav_subgroups))
        self.assertCountEqual(home_links.nav_urls, reference.symbol_urls.values())
        for module, categories in catalog.items():
            for category, keys in categories.items():
                for key in keys:
                    with self.subTest(api=key):
                        links = PageLinks(pages[reference.url(key)])
                        self.assertEqual([group["data-group"] for group in links.nav_groups if "open" in group], [module])
                        self.assertEqual([(group["data-module"], group["data-category"])
                                          for group in links.nav_subgroups if "open" in group], [(module, category)])
                        self.assertCountEqual(links.nav_urls, [posixpath.basename(url) for url in reference.symbol_urls.values()])
        self.assert_local_links_resolve(pages)

    def test_authored_api_groups_reject_missing_duplicate_and_misclassified_symbols(self):
        project, catalog = self.write_group_fixture()
        invalid = {}
        value = deepcopy(catalog)
        del value["RHI"]
        invalid["missing module"] = value
        value = deepcopy(catalog)
        value["GPU"] = value.pop("RHI")
        invalid["unknown module"] = value
        value = deepcopy(catalog)
        del value["dyf"]["렌더링"]
        invalid["missing symbol"] = value
        value = deepcopy(catalog)
        value["dyf"]["렌더링"].append("dyf::Renderer")
        invalid["duplicate in category"] = value
        value = deepcopy(catalog)
        value["dyf"]["그리기"].append("dyf::Renderer")
        invalid["duplicate across categories"] = value
        value = deepcopy(catalog)
        value["dyf"]["렌더링"] = ["dyf::RHI::Device"]
        value["RHI"]["장치"] = ["dyf::Renderer"]
        invalid["symbols in wrong modules"] = value
        value = deepcopy(catalog)
        value["dyf"]["렌더링"] = ["dyf::MissingRenderer"]
        invalid["unknown symbol"] = value
        with patch.object(generate, "ROOT", project), patch.object(generate, "HERE", project):
            for name, value in invalid.items():
                with self.subTest(invalid=name):
                    (project / "api_groups.json").write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
                    with self.assertRaises(ValueError) as caught:
                        generate.Reference()
                    self.assertTrue(str(caught.exception).strip())

    def test_api_groups_fallback_without_catalog_and_ignore_catalog_for_custom_headers(self):
        project, _ = self.write_group_fixture()
        self.write_header("Fixture.h", "namespace fixture { struct Entry { int value; }; }\n")
        with patch.object(generate, "ROOT", project), patch.object(generate, "HERE", project):
            reference = generate.Reference()
            self.assertNotIn("api_groups.json", reference.authored_hashes)
            for module, values in reference.by_module.items():
                self.assertEqual(set(reference.api_groups[module]), {"API"})
                self.assertCountEqual(reference.api_groups[module]["API"], [key for key, _ in values])
            (project / "api_groups.json").write_text("invalid JSON", encoding="utf-8")
            custom = generate.Reference(headers=self.headers)
            self.assertEqual(custom.api_groups, {"기본": {"API": ["Entry"]}})
            self.assertNotIn("api_groups.json", custom.authored_hashes)
            self.assert_local_links_resolve(custom.build())

    def test_authored_member_contracts_and_guides_have_valid_links(self):
        project = Path(self.temporary.name)
        headers = project / "src" / "Public"
        headers.mkdir(parents=True)
        (headers / "Widget.h").write_text("namespace sample { struct Widget { int size; bool Start(int count); }; }", encoding="utf-8")
        catalog = {"sample::Widget": {"summary": "위젯 설명", "members": {"size": {"summary": "크기", "remarks": ["0보다 커야 합니다."]}, "Start": {"summary": "시작", "returns": "성공 여부", "parameters": [["count", "개수"]]}}}}
        catalog_path = project / "frontend_reference.json"
        catalog_path.write_text(json.dumps(catalog, ensure_ascii=False), encoding="utf-8")
        (project / "guides.json").write_text(json.dumps([{"slug": "quickstart", "title": "시작", "summary": "가이드", "sections": [{"id": "use", "title": "사용", "paragraphs": ["[Widget](api:sample::Widget)"]}]}]), encoding="utf-8")
        with patch.object(generate, "ROOT", project), patch.object(generate, "HERE", project):
            reference = generate.Reference()
            pages = reference.build()
            self.assertIn("성공 여부", pages["component/api-widget.html"])
            self.assertIn("매개 변수", pages["component/api-widget.html"])
            self.assertIn("0보다 커야 합니다.", pages["component/api-widget.html"])
            self.assertIn('href="api-widget.html"', pages["component/quickstart.html"])
            self.assertIn('"url":"component/api-widget.html#member-start"', pages["component/assets/search-index.js"])
            self.assert_local_links_resolve(pages)
            catalog["sample::Widget"]["members"]["RemovedMethod"] = {"summary": "stale"}
            catalog_path.write_text(json.dumps(catalog), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Authored members no longer exist"):
                generate.Reference()

    def test_check_detects_changes_and_stale_pages_without_writing(self):
        header = self.write_header("Old.h", "namespace project::RHI { struct OldApi { int value; }; }\n")
        output = Path(self.temporary.name) / "output"
        original_reference = generate.Reference
        with patch.object(generate, "OUTPUT", output), patch.object(generate, "Reference", side_effect=lambda: original_reference(headers=self.headers)):
            def run(*args):
                with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    return generate.main(list(args))

            self.assertEqual(run(), 0)
            self.assertEqual(run("--check"), 0)
            written = {path.relative_to(output).as_posix(): path.read_bytes() for path in output.rglob("*") if path.is_file()}
            header.write_text("namespace project::RHI { struct OldApi { float changed; }; }\n", encoding="utf-8")
            self.assertEqual(run("--check"), 1)
            self.assertEqual(written, {path.relative_to(output).as_posix(): path.read_bytes() for path in output.rglob("*") if path.is_file()})
            self.assertEqual(run(), 0)
            self.assertEqual(run("--check"), 0)

            header.unlink()
            self.write_header("New.h", "namespace project::RHI { struct NewApi { bool ready; }; }\n")
            authored = output / "component" / "authored.html"
            authored.write_text("<!doctype html><title>직접 작성한 문서</title>\n", encoding="utf-8")
            old_pages = set(output.rglob("*.html"))
            self.assertEqual(run("--check"), 1)
            self.assertEqual(old_pages, set(output.rglob("*.html")))
            self.assertEqual(run(), 0)
            self.assertTrue(authored.exists())
            generated_content = "\n".join(path.read_text(encoding="utf-8") for path in output.rglob("*.html"))
            self.assertNotIn("OldApi", generated_content)
            self.assertIn("NewApi", generated_content)
            self.assertEqual(run("--check"), 0)


if __name__ == "__main__":
    unittest.main()
