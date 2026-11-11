#include "Scripting/DSLParser.h"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <utility>

namespace engine {

namespace {

struct Token {
    enum class Kind : uint8_t { Identifier, Number, String, Duration, Symbol, End } kind = Kind::End;
    std::string text;   // identifier text / string contents / symbol
    double number = 0.0; // Number value, Duration in seconds
    int line = 0;
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : m_source(source) {}

    [[nodiscard]] Token next() {
        skipTrivia();
        Token token;
        token.line = m_line;
        if (m_pos >= m_source.size()) {
            token.kind = Token::Kind::End;
            return token;
        }
        const char c = m_source[m_pos];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const size_t start = m_pos;
            while (m_pos < m_source.size()) {
                const char d = m_source[m_pos];
                if (std::isalnum(static_cast<unsigned char>(d)) || d == '_') {
                    ++m_pos;
                } else {
                    break;
                }
            }
            // Duration literal: <number><unit> with no space between
            // (10s, 500ms, 2m). Only when the identifier scan consumed
            // digits too -- handled below in the number branch instead.
            token.kind = Token::Kind::Identifier;
            token.text = std::string(m_source.substr(start, m_pos - start));
            return token;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && m_pos + 1 < m_source.size() && std::isdigit(static_cast<unsigned char>(m_source[m_pos + 1])))) {
            const size_t start = m_pos;
            bool dot = false;
            while (m_pos < m_source.size()) {
                const char d = m_source[m_pos];
                if (std::isdigit(static_cast<unsigned char>(d))) {
                    ++m_pos;
                } else if (d == '.' && !dot) {
                    dot = true;
                    ++m_pos;
                } else {
                    break;
                }
            }
            const std::string digits(m_source.substr(start, m_pos - start));
            token.number = std::strtod(digits.c_str(), nullptr);
            // Duration unit immediately after the digits.
            if (m_pos < m_source.size()) {
                const std::string unit(m_source.substr(m_pos, 2));
                if (unit == "ms") {
                    m_pos += 2;
                    token.kind = Token::Kind::Duration;
                    token.number /= 1000.0;
                    return token;
                }
                const char u = m_source[m_pos];
                if (u == 's') {
                    ++m_pos;
                    token.kind = Token::Kind::Duration;
                    return token;
                }
                if (u == 'm') {
                    ++m_pos;
                    token.kind = Token::Kind::Duration;
                    token.number *= 60.0;
                    return token;
                }
            }
            token.kind = Token::Kind::Number;
            return token;
        }
        if (c == '"') {
            ++m_pos;
            std::string value;
            bool closed = false;
            while (m_pos < m_source.size()) {
                const char d = m_source[m_pos];
                if (d == '"') {
                    closed = true;
                    ++m_pos;
                    break;
                }
                if (d == '\n') {
                    break;
                }
                if (d == '\\' && m_pos + 1 < m_source.size()) {
                    const char e = m_source[m_pos + 1];
                    value.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
                    m_pos += 2;
                    continue;
                }
                value.push_back(d);
                ++m_pos;
            }
            if (!closed) {
                m_errors.push_back("line " + std::to_string(token.line) + ": unterminated string literal");
            }
            token.kind = Token::Kind::String;
            token.text = std::move(value);
            return token;
        }
        // Multi-character operators first.
        const std::string two = m_pos + 1 < m_source.size() ? std::string(m_source.substr(m_pos, 2)) : "";
        if (two == "==" || two == "!=" || two == ">=" || two == "<=" || two == "&&" || two == "||") {
            m_pos += 2;
            token.kind = Token::Kind::Symbol;
            token.text = two;
            return token;
        }
        ++m_pos;
        token.kind = Token::Kind::Symbol;
        token.text = std::string(1, c);
        return token;
    }

    [[nodiscard]] const std::vector<std::string>& errors() const { return m_errors; }

private:
    void skipTrivia() {
        for (;;) {
            while (m_pos < m_source.size()) {
                const char c = m_source[m_pos];
                if (c == '\n') {
                    ++m_line;
                    ++m_pos;
                } else if (c == ' ' || c == '\t' || c == '\r') {
                    ++m_pos;
                } else {
                    break;
                }
            }
            if (m_pos + 1 < m_source.size() && m_source[m_pos] == '/' && m_source[m_pos + 1] == '/') {
                while (m_pos < m_source.size() && m_source[m_pos] != '\n') {
                    ++m_pos;
                }
                continue;
            }
            break;
        }
    }

    std::string_view m_source;
    size_t m_pos = 0;
    int m_line = 1;
    std::vector<std::string> m_errors;
};

class Parser {
public:
    explicit Parser(std::string_view source) : m_lexer(source) { advance(); }

    [[nodiscard]] DSLParseResult run() {
        DSLParseResult result;
        if (!expectIdentifier("from") || !expectIdentifier("start") || !expectSymbol("(") || !expectSymbol(")")) {
            fail("program must begin with 'from start()'");
        } else {
            auto body = parseStatements();
            if (body.has_value()) {
                result.program.statements = std::move(*body);
            }
            if (!expectIdentifier("script") || !expectSymbol("(") || !expectIdentifier("end") || !expectSymbol(")")) {
                fail("program must end with 'script(end)'");
            }
            if (m_token.kind != Token::Kind::End) {
                fail("unexpected content after 'script(end)'");
            }
        }
        result.errors = m_lexer.errors();
        result.errors.insert(result.errors.end(), m_errors.begin(), m_errors.end());
        result.program.valid = result.errors.empty();
        return result;
    }

private:
    void advance() { m_token = m_lexer.next(); }

    void fail(const std::string& what) {
        m_errors.push_back("line " + std::to_string(m_token.line) + ": " + what);
    }

    [[nodiscard]] bool atSymbol(std::string_view symbol) const {
        return m_token.kind == Token::Kind::Symbol && m_token.text == symbol;
    }
    [[nodiscard]] bool atIdentifier(std::string_view id) const {
        return m_token.kind == Token::Kind::Identifier && m_token.text == id;
    }
    [[nodiscard]] bool atEndOfBlock() const { return atSymbol("}") || m_token.kind == Token::Kind::End; }

    bool expectSymbol(std::string_view symbol) {
        if (!atSymbol(symbol)) {
            fail(std::string("expected '") + std::string(symbol) + "'");
            return false;
        }
        advance();
        return true;
    }
    bool expectIdentifier(std::string_view id) {
        if (!atIdentifier(id)) {
            fail(std::string("expected '") + std::string(id) + "'");
            return false;
        }
        advance();
        return true;
    }

    // Statement block between braces (already entered when at '{').
    std::optional<std::vector<DSLNode>> parseBlock() {
        if (!expectSymbol("{")) {
            return std::nullopt;
        }
        auto body = parseStatements();
        if (!body.has_value()) {
            return std::nullopt;
        }
        if (!expectSymbol("}")) {
            return std::nullopt;
        }
        return body;
    }

    std::optional<std::vector<DSLNode>> parseStatements() {
        std::vector<DSLNode> out;
        while (!atEndOfBlock() && !atIdentifier("script")) {
            auto node = parseStatement();
            if (!node.has_value()) {
                return std::nullopt;
            }
            out.push_back(std::move(*node));
        }
        return out;
    }

    std::optional<DSLNode> parseStatement() {
        if (m_token.kind == Token::Kind::Identifier) {
            const std::string word = m_token.text;
            if (word == "part") {
                return parsePart();
            }
            if (word == "on_input" || word == "on_hold" || word == "on_touch" || word == "on_collide" ||
                word == "on_collision" || word == "on_interact") {
                return parseEvent(word);
            }
            if (word == "every" || word == "for") {
                return parseTemporal(word);
            }
            if (word == "if") {
                return parseIf();
            }
            if (word == "inhand") {
                return parseInhand();
            }
            if (word == "play_sound" || word == "emit_sound" || word == "print") {
                return parseSoundCall(word);
            }
            if (word == "position") {
                return parsePosition();
            }
            return parseAssignment(word);
        }
        fail(m_token.kind == Token::Kind::End ? "unexpected end of script" : "expected a statement");
        return std::nullopt;
    }

    std::optional<std::string> parseStringArg(const char* context) {
        if (m_token.kind != Token::Kind::String) {
            fail(std::string("expected a quoted name in ") + context);
            return std::nullopt;
        }
        std::string value = m_token.text;
        advance();
        return value;
    }

    // Event name argument: "zone" or part("mesh") (part form resolves
    // to the same name in the interpreter).
    std::optional<std::string> parseTargetArg(const char* context) {
        if (m_token.kind == Token::Kind::String) {
            return parseStringArg(context);
        }
        if (atIdentifier("part")) {
            advance();
            if (!expectSymbol("(")) {
                return std::nullopt;
            }
            auto inner = parseStringArg(context);
            if (!inner.has_value() || !expectSymbol(")")) {
                return std::nullopt;
            }
            return inner;
        }
        if (m_token.kind == Token::Kind::Identifier) {
            std::string value = m_token.text;
            advance();
            return value;
        }
        fail(std::string("expected a quoted name in ") + context);
        return std::nullopt;
    }

    std::optional<DSLNode> parsePart() {
        const int line = m_token.line;
        advance(); // part
        if (!expectSymbol("(")) {
            return std::nullopt;
        }
        auto name = parseStringArg("part(...)");
        if (!name.has_value() || !expectSymbol(")")) {
            return std::nullopt;
        }
        DSLNode node;
        node.name = *name;
        node.line = line;
        if (atSymbol("{")) {
            node.kind = DSLNodeKind::PartBlock;
            auto block = parseBlock();
            if (!block.has_value()) {
                return std::nullopt;
            }
            node.body = std::move(*block);
            return node;
        }
        if (!expectSymbol(".")) {
            return std::nullopt;
        }
        if (m_token.kind != Token::Kind::Identifier) {
            fail("expected a property or call after 'part(...).'");
            return std::nullopt;
        }
        const std::string member = m_token.text;
        advance();
        if (atSymbol("(")) {
            // call form: destroy() / freeze() / scale(x)
            advance();
            if (member == "scale") {
                if (m_token.kind != Token::Kind::Identifier) {
                    fail("scale() expects an axis (x, y or z)");
                    return std::nullopt;
                }
                node.extra = m_token.text;
                advance();
                if (!expectSymbol(")") || !expectSymbol("=")) {
                    return std::nullopt;
                }
                node.kind = DSLNodeKind::PartAssign;
                node.property = "scale";
                auto value = parseExpression();
                if (!value.has_value()) {
                    return std::nullopt;
                }
                node.expr = std::move(*value);
                return node;
            }
            if (member != "destroy" && member != "freeze") {
                fail("unknown part() call '" + member + "'");
                return std::nullopt;
            }
            if (!expectSymbol(")")) {
                return std::nullopt;
            }
            node.kind = DSLNodeKind::PartCall;
            node.extra = member;
            return node;
        }
        if (!expectSymbol("=")) {
            return std::nullopt;
        }
        node.kind = DSLNodeKind::PartAssign;
        node.property = member;
        auto value = parseExpression();
        if (!value.has_value()) {
            return std::nullopt;
        }
        node.expr = std::move(*value);
        return node;
    }

    std::optional<DSLNode> parseEvent(const std::string& word) {
        const int line = m_token.line;
        advance();
        if (!expectSymbol("(")) {
            return std::nullopt;
        }
        auto target = parseTargetArg("event");
        if (!target.has_value() || !expectSymbol(")")) {
            return std::nullopt;
        }
        DSLNode node;
        node.kind = DSLNodeKind::EventBlock;
        node.line = line;
        node.name = *target;
        if (word == "on_input") {
            node.event = DSLEventKind::InputPress;
        } else if (word == "on_hold") {
            node.event = DSLEventKind::InputHold;
        } else if (word == "on_touch") {
            node.event = DSLEventKind::Touch;
        } else if (word == "on_collide" || word == "on_collision") {
            node.event = DSLEventKind::Collide;
        } else {
            node.event = DSLEventKind::Interact;
        }
        auto block = parseBlock();
        if (!block.has_value()) {
            return std::nullopt;
        }
        node.body = std::move(*block);
        return node;
    }

    std::optional<DSLNode> parseTemporal(const std::string& word) {
        const int line = m_token.line;
        advance();
        if (!expectSymbol("(")) {
            return std::nullopt;
        }
        auto duration = parseExpression();
        if (!duration.has_value() || !expectSymbol(")")) {
            return std::nullopt;
        }
        DSLNode node;
        node.kind = word == "every" ? DSLNodeKind::Every : DSLNodeKind::For;
        node.line = line;
        node.expr = std::move(*duration);
        auto block = parseBlock();
        if (!block.has_value()) {
            return std::nullopt;
        }
        node.body = std::move(*block);
        return node;
    }

    std::optional<DSLNode> parseIf() {
        const int line = m_token.line;
        advance(); // if
        DSLNode node;
        node.kind = DSLNodeKind::If;
        node.line = line;
        if (atSymbol("(")) {
            advance();
            auto condition = parseExpression();
            if (!condition.has_value() || !expectSymbol(")")) {
                return std::nullopt;
            }
            node.hasCondition = true;
            node.expr = std::move(*condition);
        }
        auto block = parseBlock();
        if (!block.has_value()) {
            return std::nullopt;
        }
        node.body = std::move(*block);
        return node;
    }

    std::optional<DSLNode> parseInhand() {
        const int line = m_token.line;
        advance(); // inhand
        if (!expectSymbol("(")) {
            return std::nullopt;
        }
        auto name = parseStringArg("inhand(...)");
        if (!name.has_value() || !expectSymbol(")")) {
            return std::nullopt;
        }
        DSLNode node;
        node.line = line;
        node.name = *name;
        if (atSymbol("{")) {
            node.kind = DSLNodeKind::InhandBlock;
            auto block = parseBlock();
            if (!block.has_value()) {
                return std::nullopt;
            }
            node.body = std::move(*block);
            return node;
        }
        node.kind = DSLNodeKind::InhandCall;
        return node;
    }

    std::optional<DSLNode> parseSoundCall(const std::string& word) {
        const int line = m_token.line;
        advance();
        if (!expectSymbol("(")) {
            return std::nullopt;
        }
        auto name = parseStringArg("sound call");
        if (!name.has_value() || !expectSymbol(")")) {
            return std::nullopt;
        }
        DSLNode node;
        node.kind = DSLNodeKind::SoundCall;
        node.line = line;
        node.extra = word;
        node.name = *name;
        return node;
    }

    std::optional<DSLNode> parsePosition() {
        const int line = m_token.line;
        advance(); // position
        DSLNode node;
        node.line = line;
        if (atSymbol("(")) { // position("zone") { } guard
            advance();
            auto name = parseStringArg("position(...)");
            if (!name.has_value() || !expectSymbol(")")) {
                return std::nullopt;
            }
            node.kind = DSLNodeKind::PositionBlock;
            node.name = *name;
            auto block = parseBlock();
            if (!block.has_value()) {
                return std::nullopt;
            }
            node.body = std::move(*block);
            return node;
        }
        if (!expectSymbol("=")) {
            return std::nullopt;
        }
        if (!expectIdentifier("reset")) {
            fail("only 'position = reset' is supported");
            return std::nullopt;
        }
        node.kind = DSLNodeKind::Assign;
        node.name = "position";
        node.property = "reset";
        return node;
    }

    std::optional<DSLNode> parseAssignment(const std::string& word) {
        const int line = m_token.line;
        advance(); // variable name
        DSLNode node;
        node.line = line;
        node.kind = DSLNodeKind::Assign;
        node.name = word;
        if (!expectSymbol("=")) {
            fail("expected '=' after '" + word + "'");
            return std::nullopt;
        }
        auto value = parseExpression();
        if (!value.has_value()) {
            return std::nullopt;
        }
        node.expr = std::move(*value);
        return node;
    }

    // Expression grammar, lowest to highest precedence:
    // || -> && -> comparison -> additive -> multiplicative -> unary.
    std::optional<DSLExpr> parseExpression() { return parseOr(); }

    std::optional<DSLExpr> parseOr() {
        auto lhs = parseAnd();
        if (!lhs.has_value()) {
            return std::nullopt;
        }
        while (atSymbol("||")) {
            advance();
            auto rhs = parseAnd();
            if (!rhs.has_value()) {
                return std::nullopt;
            }
            lhs = binary("||", std::move(*lhs), std::move(*rhs));
        }
        return lhs;
    }
    std::optional<DSLExpr> parseAnd() {
        auto lhs = parseComparison();
        if (!lhs.has_value()) {
            return std::nullopt;
        }
        while (atSymbol("&&")) {
            advance();
            auto rhs = parseComparison();
            if (!rhs.has_value()) {
                return std::nullopt;
            }
            lhs = binary("&&", std::move(*lhs), std::move(*rhs));
        }
        return lhs;
    }
    std::optional<DSLExpr> parseComparison() {
        auto lhs = parseAdditive();
        if (!lhs.has_value()) {
            return std::nullopt;
        }
        while (atSymbol("==") || atSymbol("!=") || atSymbol(">=") || atSymbol("<=") || atSymbol(">") || atSymbol("<")) {
            const std::string op = m_token.text;
            advance();
            auto rhs = parseAdditive();
            if (!rhs.has_value()) {
                return std::nullopt;
            }
            lhs = binary(op, std::move(*lhs), std::move(*rhs));
        }
        return lhs;
    }
    std::optional<DSLExpr> parseAdditive() {
        auto lhs = parseMultiplicative();
        if (!lhs.has_value()) {
            return std::nullopt;
        }
        while (atSymbol("+") || atSymbol("-")) {
            const std::string op = m_token.text;
            advance();
            auto rhs = parseMultiplicative();
            if (!rhs.has_value()) {
                return std::nullopt;
            }
            lhs = binary(op, std::move(*lhs), std::move(*rhs));
        }
        return lhs;
    }
    std::optional<DSLExpr> parseMultiplicative() {
        auto lhs = parseUnary();
        if (!lhs.has_value()) {
            return std::nullopt;
        }
        while (atSymbol("*") || atSymbol("/") || atSymbol("%")) {
            const std::string op = m_token.text;
            advance();
            auto rhs = parseUnary();
            if (!rhs.has_value()) {
                return std::nullopt;
            }
            lhs = binary(op, std::move(*lhs), std::move(*rhs));
        }
        return lhs;
    }
    std::optional<DSLExpr> parseUnary() {
        if (atSymbol("-") || atSymbol("!")) {
            const std::string op = m_token.text;
            advance();
            auto operand = parseUnary();
            if (!operand.has_value()) {
                return std::nullopt;
            }
            DSLExpr expr;
            expr.kind = DSLExpr::Kind::Unary;
            expr.op = op;
            expr.args.push_back(std::move(*operand));
            return expr;
        }
        return parsePrimary();
    }
    std::optional<DSLExpr> parsePrimary() {
        DSLExpr expr;
        if (m_token.kind == Token::Kind::Number || m_token.kind == Token::Kind::Duration) {
            expr.kind = DSLExpr::Kind::Number;
            expr.number = m_token.number;
            advance();
            return expr;
        }
        if (m_token.kind == Token::Kind::String) {
            expr.kind = DSLExpr::Kind::Text;
            expr.text = m_token.text;
            advance();
            return expr;
        }
        if (m_token.kind == Token::Kind::Identifier) {
            expr.kind = DSLExpr::Kind::Variable;
            expr.text = m_token.text;
            advance();
            return expr;
        }
        if (atSymbol("(")) {
            advance();
            auto inner = parseExpression();
            if (!inner.has_value() || !expectSymbol(")")) {
                return std::nullopt;
            }
            return inner;
        }
        fail("expected a number, name or '(' in expression");
        return std::nullopt;
    }

    static DSLExpr binary(const std::string& op, DSLExpr&& lhs, DSLExpr&& rhs) {
        DSLExpr expr;
        expr.kind = DSLExpr::Kind::Binary;
        expr.op = op;
        expr.args.push_back(std::move(lhs));
        expr.args.push_back(std::move(rhs));
        return expr;
    }

    Lexer m_lexer;
    Token m_token;
    std::vector<std::string> m_errors;
};

} // namespace

DSLParseResult parseBackroomsScript(std::string_view source) {
    Parser parser(source);
    return parser.run();
}

} // namespace engine
