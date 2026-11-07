#pragma once
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

bool resolve_project_inputs(const std::vector<std::string>& positional,
                            ProjectInputs& out, std::string& error);

bool check_output_target(const std::string& output,
                         const std::vector<std::string>& source_paths,
                         std::string& error);

bool load_program_sources(const ProjectInputs& inputs,
                          std::vector<SourceUnit>& out,
                          std::string& error,
                          const std::function<bool()>& resource_check = {});

}

