#pragma once
// Written by Grok
#include "parser.hpp"
#include "lexer.hpp"
#include <string>
#include <vector>
#include <functional>

namespace csy {

struct SourceUnit {
    std::string path;
    std::string source;
    LexResult lexed;
    ParseResult parsed;
};

struct ProjectInputs {
    std::string entry;
    std::vector<std::string> sources;
    std::string output;
    unsigned threads = 1;
    bool keep_tokens = false;
};

// A deliberately small, stable .csp format: quoted entry/sources/output keys.
bool resolve_project_inputs(const std::vector<std::string>& positional,
                            ProjectInputs& out, std::string& error);

// Reads, lexes and parses each input exactly once, recursively adding #connect files.
bool load_program_sources(const ProjectInputs& inputs,
                          std::vector<SourceUnit>& out,
                          std::string& error,
                          const std::function<bool()>& resource_check = {});

} // namespace csy
