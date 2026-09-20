// External scanner for the ryspec grammar.
//
// It supplies the two things TOML cannot express with regular tokens:
//
//   * `_line_ending_or_eof` -- a zero-width marker that a line has ended,
//     which is how a key/value pair or a table header is held to one line
//     without making the final line of a file a special case.
//   * the multi-line string tokens -- a run of quote characters is content
//     or terminator depending on how long the run is, which needs a count
//     the lexer's regular tokens cannot keep.
//
// Adapted from tree-sitter-toml (MIT), Copyright (c) Ika <ikatyang@gmail.com>
// and the tree-sitter-grammars organisation. The delimiter-run scan differs:
// this one counts the whole run so that a run of six or more quotes -- three
// or more content quotes against a terminator -- is rejected rather than
// split into content.

#include "tree_sitter/parser.h"

typedef enum {
    LINE_ENDING_OR_EOF,
    MULTILINE_BASIC_STRING_CONTENT,
    MULTILINE_BASIC_STRING_END,
    MULTILINE_LITERAL_STRING_CONTENT,
    MULTILINE_LITERAL_STRING_END,
} TokenType;

void *tree_sitter_ryspec_external_scanner_create(void) { return NULL; }

void tree_sitter_ryspec_external_scanner_destroy(void *payload) {}

unsigned tree_sitter_ryspec_external_scanner_serialize(void *payload, char *buffer) { return 0; }

void tree_sitter_ryspec_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {}

// Scans a run of `delimiter` characters inside a multi-line string.
//
// A run of one or two delimiters is content. A run of three to five closes the
// string, the leading nought-to-two of them being content. A run of six or more
// is invalid TOML, and is left for the parser to report.
static bool scan_delimiter_run(TSLexer *lexer, const bool *valid_symbols, int32_t delimiter,
                               TokenType content_symbol, TokenType end_symbol) {
    if (!valid_symbols[end_symbol] || lexer->lookahead != delimiter) {
        return false;
    }

    lexer->advance(lexer, false);  // 1
    if (lexer->lookahead != delimiter) {
        lexer->result_symbol = content_symbol;
        return true;
    }

    // One delimiter: the fallback end for a run of four or five, where the
    // leading delimiters are content and the last three close the string.
    lexer->mark_end(lexer);

    lexer->advance(lexer, false);  // 2
    if (lexer->lookahead != delimiter) {
        lexer->mark_end(lexer);
        lexer->result_symbol = content_symbol;
        return true;
    }

    lexer->advance(lexer, false);  // 3
    if (lexer->lookahead != delimiter) {
        lexer->mark_end(lexer);
        lexer->result_symbol = end_symbol;
        return true;
    }

    lexer->advance(lexer, false);  // 4
    if (lexer->lookahead != delimiter) {
        lexer->result_symbol = content_symbol;  // one content delimiter, then the end
        return true;
    }

    lexer->advance(lexer, false);  // 5
    if (lexer->lookahead != delimiter) {
        lexer->result_symbol = content_symbol;  // one now, one more on the next scan
        return true;
    }

    return false;  // six or more
}

bool tree_sitter_ryspec_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    if (scan_delimiter_run(lexer, valid_symbols, '"', MULTILINE_BASIC_STRING_CONTENT,
                           MULTILINE_BASIC_STRING_END) ||
        scan_delimiter_run(lexer, valid_symbols, '\'', MULTILINE_LITERAL_STRING_CONTENT,
                           MULTILINE_LITERAL_STRING_END)) {
        return true;
    }

    if (valid_symbols[LINE_ENDING_OR_EOF]) {
        lexer->result_symbol = LINE_ENDING_OR_EOF;

        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            lexer->advance(lexer, true);
        }

        if (lexer->lookahead == 0 || lexer->lookahead == '\n') {
            return true;
        }

        if (lexer->lookahead == '\r') {
            lexer->advance(lexer, true);
            if (lexer->lookahead == '\n') {
                return true;
            }
        }
    }

    return false;
}
