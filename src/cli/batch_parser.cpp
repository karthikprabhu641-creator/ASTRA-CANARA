#include "kvdb/cli/batch_parser.hpp"
#include <fstream>
#include <sstream>
#include <cctype>

namespace kvdb::cli {

namespace {

// Helper to skip whitespace characters (' ' and '\t')
void skip_whitespace(const std::string& str, std::size_t& pos) {
    while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t')) {
        pos++;
    }
}

// Extracts a single token, respecting quotes ("...") and backslash escapes (\ )
bool extract_token(const std::string& line, std::size_t& pos, std::string& token) {
    skip_whitespace(line, pos);
    if (pos >= line.size()) {
        return false;
    }

    token.clear();

    if (line[pos] == '"') {
        // Quoted string
        pos++; // Skip opening quote
        bool escaped = false;
        while (pos < line.size()) {
            char c = line[pos++];
            if (escaped) {
                switch (c) {
                    case 'n': token += '\n'; break;
                    case 'r': token += '\r'; break;
                    case 't': token += '\t'; break;
                    case '\\': token += '\\'; break;
                    case '"': token += '"'; break;
                    case ' ': token += ' '; break;
                    default: token += '\\'; token += c; break;
                }
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                // Closing quote found
                return true;
            } else {
                token += c;
            }
        }
        return false; // Unterminated quote
    }

    // Unquoted token: read until unescaped space/tab
    bool escaped = false;
    while (pos < line.size()) {
        char c = line[pos];
        if (escaped) {
            switch (c) {
                case 'n': token += '\n'; break;
                case 'r': token += '\r'; break;
                case 't': token += '\t'; break;
                case '\\': token += '\\'; break;
                case ' ': token += ' '; break;
                case '"': token += '"'; break;
                default: token += '\\'; token += c; break;
            }
            escaped = false;
            pos++;
        } else if (c == '\\') {
            escaped = true;
            pos++;
        } else if (c == ' ' || c == '\t') {
            break; // Whitespace delimiter
        } else {
            token += c;
            pos++;
        }
    }

    return !escaped;
}

} // namespace

std::string BatchParser::unescape(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    bool escaped = false;

    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (escaped) {
            switch (c) {
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case '\\': result += '\\'; break;
                case ' ': result += ' '; break;
                case '"': result += '"'; break;
                default: result += '\\'; result += c; break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else {
            result += c;
        }
    }
    if (escaped) {
        result += '\\';
    }
    return result;
}

std::string BatchParser::escape(const std::string& input) {
    std::string result;
    result.reserve(input.size() * 2);
    for (char c : input) {
        switch (c) {
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\\': result += "\\\\"; break;
            case '"':  result += "\\\""; break;
            case ' ':  result += "\\ "; break;
            default:   result += c; break;
        }
    }
    return result;
}

Status BatchParser::parse_line(const std::string& line, Operation& out_op, bool& is_empty_or_comment) {
    is_empty_or_comment = false;
    std::size_t pos = 0;
    skip_whitespace(line, pos);

    if (pos >= line.size() || line[pos] == '#') {
        is_empty_or_comment = true;
        return Status::Ok;
    }

    // Extract opcode (PUT or DEL)
    std::string op_type;
    if (!extract_token(line, pos, op_type)) {
        return Status::InvalidArgument;
    }

    // Extract key
    std::string key;
    if (!extract_token(line, pos, key)) {
        return Status::InvalidArgument;
    }

    if (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') {
        return Status::InvalidArgument;
    }

    if (key.empty() || key.size() > MAX_KEY_SIZE) {
        return Status::InvalidArgument;
    }

    if (op_type == "PUT") {
        skip_whitespace(line, pos);
        std::string value;

        if (pos < line.size()) {
            if (line[pos] == '"') {
                // Quoted value
                if (!extract_token(line, pos, value)) {
                    return Status::InvalidArgument;
                }
                // Verify no non-whitespace trailing data after closing quote
                skip_whitespace(line, pos);
                if (pos < line.size() && line[pos] != '#') {
                    return Status::InvalidArgument;
                }
            } else {
                // Unquoted remainder is value; unescape C-style escapes
                std::string raw_val = line.substr(pos);
                // Strip trailing carriage return if present
                if (!raw_val.empty() && raw_val.back() == '\r') {
                    raw_val.pop_back();
                }
                value = unescape(raw_val);
            }
        }

        if (value.size() > MAX_VALUE_SIZE) {
            return Status::InvalidArgument;
        }

        out_op.type = Operation::Type::Put;
        out_op.key = std::move(key);
        out_op.value = std::move(value);
        return Status::Ok;

    } else if (op_type == "DEL") {
        // DEL takes only one argument (key). Ensure no trailing non-whitespace data.
        skip_whitespace(line, pos);
        if (pos < line.size() && line[pos] != '#') {
            return Status::InvalidArgument; // Unexpected arguments after key
        }

        out_op.type = Operation::Type::Delete;
        out_op.key = std::move(key);
        out_op.value = "";
        return Status::Ok;
    }

    // Unknown opcode
    return Status::InvalidArgument;
}

Status BatchParser::parse_file(const std::string& filepath, std::vector<Operation>& out_operations) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return Status::IoError;
    }

    out_operations.clear();
    std::string line;

    while (std::getline(file, line)) {
        // Strip trailing CR if file has CRLF
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        bool is_comment_or_empty = false;
        Operation op;
        Status s = parse_line(line, op, is_comment_or_empty);
        if (!is_ok(s)) {
            out_operations.clear();
            return s; // Reject immediately
        }

        if (is_comment_or_empty) {
            continue;
        }

        if (out_operations.size() >= MAX_BATCH_OPERATIONS) {
            out_operations.clear();
            return Status::InvalidArgument; // Exceeds max batch size
        }

        out_operations.push_back(std::move(op));
    }

    if (file.bad()) {
        out_operations.clear();
        return Status::IoError;
    }

    if (out_operations.empty()) {
        return Status::InvalidArgument; // Empty batches are invalid
    }

    return Status::Ok;
}

} // namespace kvdb::cli
