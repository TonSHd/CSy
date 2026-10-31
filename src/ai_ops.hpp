#pragma once

namespace csy {

class VM;
void register_ai_builtins(VM& vm);

void set_ai_run_threads(unsigned threads);

}

