#pragma once
#include <string>

namespace csy {
namespace ccm {

int handle_command(int argc, char** argv);
void begin(int argc, char** argv);
void set_subject(const std::string& entry_path, bool run_mode, bool final_call);
void phase_check();
void user_code(bool running);
void note_rebuilt();
void finish(int exit_code);
std::string default_table_path();

enum class TableCopy { Created, Kept, NotFound, Failed };
TableCopy copy_default_table(const std::string& folder, std::string& detail);

}
}
