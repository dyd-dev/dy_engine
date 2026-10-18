"""Extract C++ header declarations and adjacent comments without external metadata.

This is a declaration scanner for the public headers, not a C++ compiler. It does
not resolve macros, inherited members, aliases, or implementation semantics.
Unsupported declarations raise an error with the source location.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import re
import textwrap


@dataclass
class Decl:
    name: str
    kind: str
    header: str
    line: int
    syntax: str
    members: list[Decl] = field(default_factory=list)
    value: str = ""
    namespace: str = ""
    comment: str = ""
    condition: str = ""
    access: str = "public"

    @property
    def page(self):
        return "api-" + slug(self.name) + ".html"


def slug(value):
    value = re.sub(r"operator\s*", "operator-", value)
    value = re.sub(r"(?<=operator-)-+", lambda match: "minus" * len(match.group()), value)
    for token, word in [("[]", "subscript"), ("()", "call"), ("|", "or"), ("&", "and"), ("=", "equal"),
                        ("!", "not"), ("~", "destructor-"), ("+", "plus"),
                        ("*", "multiply"), ("/", "divide"), ("%", "modulo"),
                        ("<", "less"), (">", "greater"), ("^", "xor")]:
        value = value.replace(token, word)
    return re.sub(r"[^a-z0-9-]+", "-", value.lower()).strip("-")


_LEX = re.compile(
    r'R"(?P<delimiter>[^\s()\\]{0,16})\([\s\S]*?\)(?P=delimiter)"'
    r'|//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
)
_TYPE = re.compile(r"(enum(?:\s+(?:class|struct))?|class|struct|union)\b(?:\s+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*))?")
_NAMESPACE = re.compile(r"\b(?:inline\s+)?namespace\s*([A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*)?\s*\{")
_FUNCTION = re.compile(
    r"(?P<name>operator\s*(?:\[\]|\(\)|[+*/%|&^~!=<>-]+)"
    r"|operator\s+[A-Za-z_][\w:<>, \t*&]*|~?[A-Za-z_]\w*)\s*(?P<opening>\()"
)


def masked(source):
    """Mask comments and literals while keeping all source offsets and newlines."""
    return _LEX.sub(lambda match: re.sub(r"[^\n]", " ", match.group()), source)


def clean(value):
    value = _LEX.sub(lambda match: re.sub(r"[^\n]", " ", match.group())
                     if match.group().startswith(("//", "/*")) else match.group(), value)
    return textwrap.dedent(value.expandtabs(4)).strip()


def compact(value):
    # Whitespace inside strings (including raw strings) is part of the API's
    # default value. Collapse only the code between literal tokens.
    pieces = []
    pending = ""
    end = 0
    for match in _LEX.finditer(value):
        pending += value[end:match.start()]
        token = match.group()
        if token.startswith(("//", "/*")):
            pending += " "
        else:
            pieces.extend((re.sub(r"\s+", " ", pending), token))
            pending = ""
        end = match.end()
    pieces.append(re.sub(r"\s+", " ", pending + value[end:]))
    return "".join(pieces).strip()


def _without_attributes(value):
    return re.sub(r"\[\[[\s\S]*?\]\]", "", value).strip()


def _declaration_mask(value):
    """Hide attributes and literals without moving declaration offsets."""
    code = re.sub(r"\[\[[\s\S]*?\]\]",
                  lambda match: re.sub(r"[^\n]", " ", match.group()), masked(value))
    while match := re.match(r"\s*template\s*<", code):
        depth = 1
        end = match.end()
        while end < len(code) and depth:
            depth += (code[end] == "<") - (code[end] == ">")
            end += 1
        if depth:
            break
        code = re.sub(r"[^\n]", " ", code[:end]) + code[end:]
    return code


def _type_match(value):
    code = _declaration_mask(value).strip()
    code = re.sub(r"\balignas\s*\([^()]*\)\s*", "", code)
    code = re.sub(r"^typedef\s+", "", code)
    return _TYPE.match(code)


def _function_match(value):
    code = _declaration_mask(value)
    for match in _FUNCTION.finditer(code):
        if match.group("name") in {"explicit", "noexcept", "decltype", "alignas", "sizeof", "requires"}:
            continue
        prefix = code[:match.start()]
        # An apparent function within a template or another parameter list is
        # part of the type/default expression, not this declaration's name.
        if prefix.count("(") != prefix.count(")") or prefix.count("<") != prefix.count(">"):
            continue
        return code, match
    return code, None


def _comment_text(value):
    if value.startswith("//"):
        return re.sub(r"^//[/!]?<?\s?", "", value).rstrip()
    value = re.sub(r"^/\*[*!]?<?", "", value)[:-2]
    return textwrap.dedent("\n".join(re.sub(r"^\s*\* ?", "", line)
                                    for line in value.splitlines())).strip()


@dataclass
class _Statement:
    start: int
    end: int
    mode: str
    body_start: int = 0
    body_end: int = 0
    signature: str = ""


class _ParseError(ValueError):
    def __init__(self, header, line, pos, reason):
        super().__init__(f"{header}:{line}: {reason}")
        self.pos = pos
        self.reason = reason


class _Parser:
    def __init__(self, source, header, diagnostics=None):
        self.source = source
        self.code = masked(source)
        self.header = header
        self.diagnostics = diagnostics
        self.directives = []
        self.conditions = []
        self.macros = []
        self.comments = [(match.start(), match.end(), match.group())
                         for match in _LEX.finditer(source)
                         if match.group().startswith(("//", "/*"))]
        if source.startswith("\ufeff"):
            self.fail(0, "UTF-8 BOM은 지원하지 않습니다. BOM 없이 저장하세요")
        self.preprocess()

    def fail(self, pos, message):
        raise _ParseError(self.header, self.line(pos), pos, message)

    def diagnostic(self, pos, syntax, reason):
        if self.diagnostics is not None:
            self.diagnostics.append({"header": self.header, "line": self.line(pos),
                                     "syntax": syntax.strip(), "reason": reason})

    def preprocess(self):
        """Keep every conditional branch; never claim a compiler configuration."""
        condition_stack = []
        masked_code = list(self.code)
        # A line-by-line scan handles continued macro definitions without matching
        # apparent directives inside comments/string literals.
        lines = self.code.splitlines(keepends=True)
        offsets = []
        cursor = 0
        for line in lines:
            offsets.append(cursor)
            cursor += len(line)
        index = 0
        while index < len(lines):
            start = offsets[index]
            if not re.match(r"[ \t]*#", lines[index]):
                index += 1
                continue
            finish = start + len(lines[index])
            while lines[index].rstrip("\r\n").endswith("\\") and index + 1 < len(lines):
                index += 1
                finish = offsets[index] + len(lines[index])
            raw = self.source[start:finish].rstrip()
            match = re.match(r"\s*#\s*(\w+)\s*(.*)", raw, re.DOTALL)
            command, argument = match.groups() if match else ("", "")
            if command in {"if", "ifdef", "ifndef"}:
                expression = ("defined(" + argument.strip() + ")" if command == "ifdef" else
                              "!defined(" + argument.strip() + ")" if command == "ifndef" else argument.strip())
                condition_stack.append([expression, expression])
            elif command == "elif" and condition_stack:
                earlier = condition_stack[-1][0]
                condition_stack[-1] = [earlier + " || " + argument.strip(),
                                       "!(" + earlier + ") && (" + argument.strip() + ")"]
            elif command == "else" and condition_stack:
                condition_stack[-1][1] = "!(" + condition_stack[-1][0] + ")"
            elif command == "endif" and condition_stack:
                condition_stack.pop()
            elif command == "define":
                macro = re.match(r"([A-Za-z_]\w*)(?:\(|\s+\S)", argument)
                if macro:
                    self.macros.append(Decl(macro.group(1), "macro", self.header, self.line(start), raw,
                                            comment=self.comment(start, finish),
                                            condition=" && ".join("(" + item[1] + ")" for item in condition_stack)))
            elif command == "undef":
                pass
            elif command == "include" and re.search(r"\.inc[>\"]", argument):
                self.diagnostic(start, raw, "포함된 .inc 파일의 선언은 이 헤더 안으로 확장하지 않습니다.")
            elif command not in {"include", "pragma", "line", "error", "warning"}:
                self.diagnostic(start, raw, "전처리 지시문은 실행하지 않습니다.")
            self.conditions.append((finish, " && ".join("(" + item[1] + ")" for item in condition_stack)))
            self.directives.append((start, finish))
            masked_code[start:finish] = ["\n" if ch == "\n" else " " for ch in self.code[start:finish]]
            index += 1
        self.code = "".join(masked_code)

    def condition(self, pos):
        result = ""
        for start, condition in self.conditions:
            if start > pos:
                break
            result = condition
        return result

    def line(self, pos):
        return self.source.count("\n", 0, pos) + 1

    def closing(self, start, limit, left="{", right="}"):
        depth = 0
        for pos in range(start, limit):
            if self.code[pos] == left:
                depth += 1
            elif self.code[pos] == right:
                depth -= 1
                if not depth:
                    return pos
        self.fail(start, f"닫히지 않은 {left}")

    def comment(self, start, end, lower=0):
        """Only adjacent leading comments and a trailing comment on this line."""
        preceding = []
        cursor = start
        for begin, finish, value in reversed(self.comments):
            if finish > cursor:
                continue
            if begin < lower:
                break
            gap = self.source[finish:cursor]
            if gap.strip() or re.search(r"\n[ \t\r]*\n", gap):
                break
            line_start = self.source.rfind("\n", 0, begin) + 1
            if self.source[line_start:begin].strip():
                break  # A preceding declaration's trailing comment is its own.
            preceding.append(_comment_text(value))
            cursor = begin
        preceding.reverse()
        line_end = self.source.find("\n", end)
        if line_end < 0:
            line_end = len(self.source)
        for begin, finish, value in self.comments:
            if begin < end:
                continue
            if begin >= line_end or self.source[end:begin].strip():
                break
            preceding.append(_comment_text(value))
            end = finish
        return "\n".join(part for part in preceding if part).strip()

    def statements(self, begin, end):
        pos = begin
        while pos < end:
            if self.code[pos].isspace() or self.code[pos] == ";":
                pos += 1
                continue
            start = pos
            access = re.match(r"(public|protected|private)\s*:", self.code[pos:end])
            if access:
                pos += access.end()
                yield _Statement(start, pos, "access", signature=access.group(1))
                continue
            macro = re.match(r"[A-Z_][A-Z_0-9]*\s*\(", self.code[pos:end])
            if macro:
                opening = pos + macro.end() - 1
                close = self.closing(opening, end, "(", ")")
                line_end = self.code.find("\n", close)
                if line_end < 0:
                    line_end = end
                after = close + 1
                while after < end and self.code[after].isspace():
                    after += 1
                if not self.code[close + 1:line_end].strip() and self.code[after:after + 1] not in {"{", ":", ";"}:
                    yield _Statement(start, close + 1, "macro")
                    pos = close + 1
                    continue
            parens = brackets = 0
            while pos < end:
                ch = self.code[pos]
                if ch == "(":
                    parens += 1
                elif ch == ")":
                    parens -= 1
                elif ch == "[":
                    brackets += 1
                elif ch == "]":
                    brackets -= 1
                if parens < 0 or brackets < 0:
                    self.fail(pos, "일치하지 않는 괄호")
                if ch == "{" and parens == 0 and brackets == 0:
                    close = self.closing(pos, end)
                    prefix = compact(self.source[start:pos])
                    bare = _without_attributes(prefix)
                    if _type_match(prefix) or _NAMESPACE.match(self.code[start:pos + 1]) or re.match(r'extern\s+"[^"]+"\s*$', prefix):
                        finish = close + 1
                        while finish < end and self.code[finish].isspace():
                            finish += 1
                        if finish < end and self.code[finish] == ";":
                            finish += 1
                        elif _type_match(prefix) and re.match(r"[A-Za-z_]\w*\s*;", self.code[finish:end]):
                            # C typedef structs and anonymous aggregate variables.
                            finish += re.match(r"[A-Za-z_]\w*\s*;", self.code[finish:end]).end()
                        else:
                            finish = close + 1
                        yield _Statement(start, finish, "block", pos + 1, close)
                        pos = finish
                        break
                    signature = self.function_signature(prefix)
                    if signature is not None:
                        # A constructor's braced member initializer precedes its body.
                        if signature != prefix and not prefix.endswith((")", "}")):
                            pos = close + 1
                            continue
                        yield _Statement(start, close + 1, "method", signature=signature + ";")
                        pos = close + 1
                        break
                    pos = close
                elif ch == ";" and parens == 0 and brackets == 0:
                    yield _Statement(start, pos + 1, "statement")
                    pos += 1
                    break
                pos += 1
            else:
                self.fail(start, "해석하지 못한 선언: " + compact(self.source[start:end])[:140])

    def statements_safe(self, begin, end):
        iterator = self.statements(begin, end)
        while True:
            try:
                yield next(iterator)
            except StopIteration:
                return
            except _ParseError as error:
                if self.diagnostics is None:
                    raise
                self.diagnostic(error.pos, self.source[error.pos:end], error.reason)
                return

    def function_signature(self, syntax):
        code, match = _function_match(syntax)
        if match is None:
            return None
        # Assignments and braced aggregate initializers are fields, even if their
        # initializer invokes a function. operator= remains a function declaration.
        opening = match.start("opening")
        assignment = re.search(r"(?<![=!<>])=(?!=)", code[:opening])
        if assignment and "operator" not in code[:opening]:
            return None
        if re.match(r"\(\s*[*&]", code[opening:]):
            return None
        # Locate the actual parameter list after attributes, then scan through
        # qualifiers such as noexcept(expr) to find a constructor's initializer.
        depth = 0
        closing = None
        for pos in range(opening, len(code)):
            if code[pos] == "(":
                depth += 1
            elif code[pos] == ")":
                depth -= 1
                if depth == 0:
                    closing = pos
                    break
        if closing is not None:
            parens = brackets = 0
            for pos in range(closing + 1, len(code)):
                ch = code[pos]
                if ch == "(":
                    parens += 1
                elif ch == ")":
                    parens -= 1
                elif ch == "[":
                    brackets += 1
                elif ch == "]":
                    brackets -= 1
                elif ch == ":" and parens == brackets == 0:
                    if code[pos - 1:pos] != ":" and code[pos + 1:pos + 2] != ":":
                        return syntax[:pos].rstrip()
        return syntax

    def function_name(self, syntax, pos):
        _, match = _function_match(syntax)
        if not match:
            self.fail(pos, "지원하지 않는 함수 선언: " + syntax)
        name = match.group("name").strip()
        if re.match(r"operator\s+[A-Za-z_]", name):
            return re.sub(r"\s+", " ", name)
        return re.sub(r"\s+", "", name)

    def make(self, statement, namespace, lower, member=False):
        raw = self.source[statement.start:statement.end]
        syntax = statement.signature or compact(raw)
        bare = _without_attributes(syntax)
        comment = self.comment(statement.start, statement.end, lower)
        if statement.mode == "macro":
            self.fail(statement.start, "매크로 호출로 생성되는 선언은 확장하지 않습니다")
        if re.match(r"static_assert\b", bare):
            return None  # A compile-time check defines no API symbol.
        if re.match(r"friend\s+(?:class|struct)\b", bare):
            return None  # A friendship declaration defines no new type.
        match = _type_match(syntax)
        if match:
            if statement.mode != "block":
                if re.fullmatch(r"(?:class|struct|union|enum(?:\s+(?:class|struct))?)\s+[\w:]+(?:\s*:\s*[\w:]+)?\s*;", bare):
                    return None  # Forward declarations have no members to document.
                self.fail(statement.start, "지원하지 않는 형식 선언: " + syntax)
            kind, name = match.groups()
            trailing = compact(self.source[statement.body_end + 1:statement.end]).rstrip(";")
            if name is None:
                name = trailing if re.fullmatch(r"[A-Za-z_]\w*", trailing) else f"anonymous-{kind}-{self.line(statement.start)}"
            kind = "enum" if kind.startswith("enum") else kind
            declaration = Decl(name, kind, self.header, self.line(statement.start), "",
                               namespace=namespace, comment=comment,
                               condition=self.condition(statement.start))
            if kind == "enum":
                declaration.members = self.enum_members(statement, namespace)
                declaration.syntax = clean(raw).rstrip(";") + ";"
            else:
                access = "private" if kind == "class" else "public"
                for item in self.statements_safe(statement.body_start, statement.body_end):
                    if item.mode == "access":
                        access = item.signature
                    elif access == "public":
                        declaration.members.extend(self.make_safe(item, namespace, statement.body_start, member=True))
                prefix = clean(self.source[statement.start:statement.body_start - 1])
                member_syntax = []
                for item in declaration.members:
                    rendered = textwrap.indent(item.syntax, "    ")
                    if item.condition and item.condition != declaration.condition:
                        rendered = "#if " + item.condition + "\n" + rendered + "\n#endif"
                    member_syntax.append(rendered)
                members = "\n".join(member_syntax)
                declaration.syntax = prefix + "\n{\n" + ("public:\n" if kind == "class" else "") + members + "\n};"
            if trailing:
                declaration.syntax = clean(raw)
                if bare.startswith("typedef ") and trailing != name:
                    return [declaration, Decl(trailing, "alias", self.header, declaration.line,
                                             declaration.syntax, namespace=namespace, comment=comment,
                                             condition=declaration.condition)]
            return declaration
        bare = _without_attributes(_declaration_mask(syntax)).strip() if re.match(r"\s*template\b", syntax) else bare
        if re.match(r"namespace\s+\w+\s*=", bare):
            name = re.match(r"namespace\s+(\w+)", bare).group(1)
            kind = "alias"
        elif bare.startswith("typedef "):
            pointer = re.search(r"\(\s*[*&]\s*(\w+)\s*\)", bare)
            match = re.search(r"\b([A-Za-z_]\w*)\s*(?:\[[^]]*\]\s*)*;\s*$", bare)
            if not (pointer or match):
                self.fail(statement.start, "typedef 이름을 찾지 못했습니다: " + syntax)
            name, kind = (pointer or match).group(1), "alias"
        elif bare.startswith("using "):
            match = re.match(r"using\s+(\w+)\s*=", bare)
            if not match:
                self.fail(statement.start, "형식 별칭 이외의 using 선언은 지원하지 않습니다: " + syntax)
            name, kind = match.group(1), "alias"
        elif self.function_signature(syntax) is not None:
            name = self.function_name(syntax, statement.start)
            kind = "method" if member and not bare.startswith("friend ") else "function"
        else:
            return self.variables(statement, namespace, lower, member)
        return Decl(name, kind, self.header, self.line(statement.start), syntax,
                    namespace=namespace, comment=comment, condition=self.condition(statement.start))

    def make_safe(self, statement, namespace, lower, member=False):
        try:
            result = self.make(statement, namespace, lower, member)
            return result if isinstance(result, list) else [result] if result is not None else []
        except _ParseError as error:
            if self.diagnostics is None:
                raise
            self.diagnostic(error.pos, self.source[statement.start:statement.end], error.reason)
            return []

    def variables(self, statement, namespace, lower, member):
        raw = self.source[statement.start:statement.end].rstrip().rstrip(";")
        code = _declaration_mask(raw)
        positions = [0]
        parens = brackets = braces = angles = 0
        for pos, ch in enumerate(code):
            if ch == "(": parens += 1
            elif ch == ")": parens -= 1
            elif ch == "[": brackets += 1
            elif ch == "]": brackets -= 1
            elif ch == "{": braces += 1
            elif ch == "}": braces -= 1
            elif ch == "<" and parens == brackets == braces == 0: angles += 1
            elif ch == ">" and angles and parens == brackets == braces == 0: angles -= 1
            elif ch == "," and parens == brackets == braces == angles == 0:
                positions.append(pos + 1)
        positions.append(len(raw) + 1)
        result = []
        base = ""
        for index, (begin, end) in enumerate(zip(positions, positions[1:])):
            part = raw[begin:end - 1].strip()
            if not part:
                self.fail(statement.start + begin, "비어 있는 변수 선언")
            prefix = re.split(r"[={;]", _without_attributes(part), maxsplit=1)[0].strip()
            pointer = re.search(r"\(\s*[*&]\s*(\w+)\s*\)", prefix)
            match = pointer or re.search(r"\b([A-Za-z_]\w*)\s*(?:\[[^]]*\]\s*)*(?::\s*[^:]+)?$", prefix)
            without_templates = prefix
            while re.search(r"<[^<>]*>", without_templates):
                without_templates = re.sub(r"<[^<>]*>", "", without_templates)
            if not match or index == 0 and match.start() == 0 or "(" in without_templates and pointer is None:
                self.fail(statement.start + begin, "변수 이름을 찾지 못했습니다: " + compact(part))
            name = match.group(1)
            if index == 0:
                base = prefix[:match.start()].strip()
                if pointer is None:
                    base = re.sub(r"(?:\s*[*&]+\s*(?:(?:const|volatile)\s*)*)+$", "", base).rstrip()
            syntax = compact((base + " " if index else "") + part) + ";"
            actual_begin = begin
            while actual_begin < end - 1 and code[actual_begin].isspace():
                actual_begin += 1
            result.append(Decl(name, "field" if member else "variable", self.header,
                               self.line(statement.start + actual_begin), syntax, namespace=namespace,
                               comment=self.comment(statement.start, statement.end, lower),
                               condition=self.condition(statement.start)))
        return result

    def enum_members(self, statement, namespace):
        members = []
        start = statement.body_start
        pos = start
        parens = brackets = braces = 0
        next_value = 0
        previous_name = ""
        conditional_start = min(
            (begin for begin, finish in self.directives
             if statement.body_start <= begin < statement.body_end
             and re.match(r"\s*#\s*(?:if|ifdef|ifndef)\b", self.source[begin:finish])),
            default=statement.body_end + 1,
        )
        while pos <= statement.body_end:
            ch = self.code[pos] if pos < statement.body_end else ","
            if ch == "(":
                parens += 1
            elif ch == ")":
                parens -= 1
            elif ch == "[":
                brackets += 1
            elif ch == "]":
                brackets -= 1
            elif ch == "{":
                braces += 1
            elif ch == "}":
                braces -= 1
            elif ch == "," and parens == brackets == braces == 0:
                while start < pos and self.code[start].isspace():
                    start += 1
                if start < pos:
                    syntax = compact(self.source[start:pos])
                    match = re.fullmatch(r"([A-Za-z_]\w*)\s*(?:=\s*(.+))?", _without_attributes(syntax))
                    if not match:
                        self.fail(start, "지원하지 않는 열거형 원소: " + syntax)
                    name, expression = match.groups()
                    if expression is not None:
                        value = expression
                        literal = re.fullmatch(r"(0[xX][\da-fA-F]+|0[bB][01]+|0[0-7]+|\d+)[uUlL]*", expression)
                        if literal:
                            number = literal.group(1)
                            next_value = int(number, 8 if number.startswith("0") and len(number) > 1 and number[1].isdigit() else 0 if number != "0" else 10)
                        else:
                            next_value = None
                    else:
                        if start >= conditional_start:
                            # Both branches are present in the source scan; walking
                            # them in sequence is not C++ enum value evaluation.
                            value = "전처리 조건에 따라 결정 (암시적)"
                        else:
                            value = f"{next_value if next_value is not None else previous_name + ' + 1'} (암시적)"
                    # Include the separating comma so its trailing comment belongs here.
                    finish = pos + 1 if pos < statement.body_end else pos
                    if pos == statement.body_end:
                        while finish > start and self.code[finish - 1].isspace():
                            finish -= 1
                    members.append(Decl(name, "value", self.header, self.line(start), syntax,
                                        value=value, namespace=namespace,
                                        comment=self.comment(start, finish, statement.body_start),
                                        condition=self.condition(start)))
                    previous_name = name
                    if next_value is not None:
                        next_value += 1
                start = pos + 1
            pos += 1
        return members

    def scope(self, begin, end, namespace):
        declarations = []
        for statement in self.statements_safe(begin, end):
            match = _NAMESPACE.match(self.code[statement.start:statement.end])
            if match:
                name = re.sub(r"\s+", "", match.group(1) or f"anonymous-{self.line(statement.start)}")
                declarations.extend(self.scope(statement.body_start, statement.body_end, (namespace + "::" if namespace else "") + name))
                continue
            if statement.mode == "block" and re.match(r'extern\s+"[^"]+"\s*\{', self.source[statement.start:statement.end]):
                declarations.extend(self.scope(statement.body_start, statement.body_end, namespace))
                continue
            declarations.extend(self.make_safe(statement, namespace, begin))
        return declarations

    def parse(self):
        return sorted(self.scope(0, len(self.code), "") + self.macros, key=lambda item: item.line)


def parse_header(path: Path, source=None, header=None, diagnostics=None) -> list[Decl]:
    path = Path(path)
    if source is None:
        source = path.read_text(encoding="utf-8")
    return _Parser(source, header or path.name, diagnostics).parse()
