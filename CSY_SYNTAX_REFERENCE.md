# CSy Language Syntax Reference

This document provides a comprehensive reference for all CSy language syntax, designed for AI learning and understanding.

## Overview

CSy is an AI-focused compiled language designed for AI development with concise, readable syntax and direct control over model architecture. It compiles to bytecode and can optionally generate native code with its own x86-64 backend.

## File Extensions

- `.csy` - CSy source files
- `.csp` - CSy project files (TOML-like configuration)
- `.cso` - Compiled CSy objects (bytecode)
- `.csyf64` - Binary Float64 array checkpoints (checksummed, see Packed Float64 Arrays)

## Comments

```csy
# Single-line comment starts with #
# All text after # on a line is ignored
```

## Module Imports

```csy
# Import a module and alias it
#inc basic as bc;
#inc ai as ai;
#inc path as path;
#inc fs as fs;
#inc nvidia as nv;  # deprecated, use #inc ai as ai
```

The `path` and `fs` modules provide opt-in general-purpose path and filesystem functions.
They do not add names to `basic`, and existing `bc::...` calls remain available.

```csy
#inc path as path;
#inc fs as fs;

config = path::join("settings", "app.json");
path::basename(config);
path::dirname(config);
path::extension(config);
path::normalize("data/../data/input.csv");
path::is_absolute("/var/tmp");

fs::exists(config);
fs::is_file(config);
fs::is_dir("data");
fs::list_dir("data");
```

- `path::join` joins two or more path strings. An absolute later path replaces the preceding path.
- `path::basename`, `dirname` and `extension` return the final component, parent component and
  extension. A filename such as `.env` has no extension.
- `path::normalize` performs lexical normalization only; it does not access the filesystem or
  resolve symbolic links.
- `fs::exists`, `fs::is_file` and `fs::is_dir` query the filesystem. Missing paths return `false`;
  inspection errors are reported.
- `fs::list_dir` returns entry names in sorted order and reports errors. `fs::create_dir` and
  `fs::remove_file` are also available; they return `true` on success and `false` on failure, and
  `remove_file` refuses directories.
- `fs::read_file`, `write_file`, `append_file` and `read_lines` are available as well. They retain
  the same return and error behavior as the corresponding `bc::` functions.
- The filesystem functions are Linux-host filesystem operations and use paths relative to the
  process working directory when given relative paths.

## Linux Process and Bash

External commands are available through separate opt-in modules:

```csy
#inc process as proc;
#inc shell as sh;

result = proc::run("printf", ["hello"]);
script_result = sh::run("printf 'hello from Bash'");
exit_status = sh::interactive();
```

- `process::run(program, arguments, options?)` executes a program directly with an argument
  vector; it does not parse arguments as shell syntax.
- `shell::run(script, options?)` runs a non-interactive Bash script using
  `/bin/bash --noprofile --norc -c`. Use it when shell syntax such as pipes or redirection is
  needed.
- `shell::interactive()` starts an interactive Bash session and returns its exit status. It
  requires an interactive terminal.
- The optional dictionary supports `cwd` (working directory), `env` (string-to-string environment
  overrides), `timeout_ms` (0 means no timeout; the maximum is 604800000), and `stream` (also
  write captured output to the current terminal). Unknown options and invalid values are errors.
- `process::run` and `shell::run` return a dictionary with `stdout`, `stderr`, `status`, and
  `timed_out`. Captured output is limited to 64 MiB total. A timeout reports status 124.

The compiler asks for approval for each new external-command call site. Option 1 approves that
call site, option 2 denies it, and option 3 trusts the project directory. Approvals are stored in
the current user's configuration directory (`$XDG_CONFIG_HOME/csyc/command-trust`, or
`~/.config/csyc/command-trust`); they are not stored in the project or its `.csp` file. A compiled
object run without a matching local approval asks again. Commands recognized as `sudo`, `doas`, `pkexec`,
`run0` or `su` also require two runtime confirmations before execution, including when they are started
through a wrapper such as `env`, `nice`, `timeout` or `xargs`, or named inside the text given to a shell
or an interpreter.

For example, this uses `sudo` to run a harmless command:

```csy
#inc process as proc;

result = proc::run("sudo", ["/usr/bin/printf", "hello\n"]);
```

During compilation, choose `1` to approve this call site, `2` to deny it, or `3` to trust the
project directory:

```text
warning: main.csy:3: external command call requires approval
  1. I know what I'm doing.
  2. NO!
  3. I know what I'm doing. DON'T warn me again in THIS project directory
Choose 1, 2, or 3:
```

Choosing `2` stops compilation with an approval-denied error and does not produce the requested
`.cso` output. This approval is for the call site; it does not inspect the command to determine
whether it is harmless. At runtime, a recognized `sudo`, `doas`, `pkexec`, `run0` or `su` call asks twice for `y` or
`yes`; declining either prompt cancels the command. Confirming allows it to run with whatever
privileges the operating system grants, so these prompts do not make destructive commands safe.

Privilege checks for commands inside Bash are best-effort, not a security boundary: they only
recognize some forms, and interactive shell wrappers can be bypassed. Do not rely on them to
prevent a command from obtaining elevated privileges.

## Using/Aliasing

```csy
# Create a short alias for a builtin or imported name
using bc::out as o;
# Now use o> instead of bc::out>
```

A `using` alias can name any builtin, or any member of an imported module or `#connect` namespace:

```csy
#inc basic as bc;
#inc ai as ai;
#connect "scripts/mathx.csy";

using bc::len as ln;
using bc::sqrt as sq;
using ai::dense as dense;
using mathx::add as plus;

bc::out>ln([1, 2, 3]);     # 3
bc::out>sq(16.0);          # 4
bc::out>bc::map(sq, [4.0, 9.0]);   # an alias can also be passed as a value
```

- The target is checked on the `using` line, so `using bc::lenn as x;` is a compile error with a
  suggestion.
- A parameter, loop variable or local variable with the same name hides the alias inside that
  function.
- An alias is optional. The common builtins also work bare: `len(x)`, `sqrt(x)`, `range(n)` and
  `isnan(x)` need no `bc::` and no `using`.
- Aliases are resolved by the compiler, so they cost nothing at run time, and native code treats
  `sq(x)` exactly like `bc::sqrt(x)`.

## Variables

CSy uses implicit typing. Variables are declared with assignment.

```csy
# Integer
x = 10;

# Float
y = 3.14;

# String
name = "CSy";

# Boolean
ready = true;

# None/null
empty = none;

# Vector/list
nums = [1, 2, 3, 4, 5];

# Dict/record
person = {"name": "Alice", "age": 30};
```

A variable has to be given a value before it is read. Reading a name at the top level before its
first assignment is a compile error (`x = x + 1;` with no earlier `x`); inside a loop it is a warning,
because an earlier round of the loop may have set it. The left side of `=` must be a variable, an
element (`v[0] = 1;`) or a field (`me::x = 1;`). A literal (`true = 5;`), a module name (`bc = 1;`
when `bc` is an `#inc` alias) and a name that lives in a module or `#connect` (`bc::len = 5;`) cannot
be assigned to, and a module name is not a value (`x = bc;` is an error; write `bc::len`).

## Data Types

### Primitive Types
- **Integer** - e.g., `42`, `-10`
- **Float** - e.g., `3.14`, `-0.5`, `2.0`
- **String** - e.g., `"hello"`, `"world"`
- **Boolean** - `true` or `false`
- **None** - `none` (null/undefined value)

### Composite Types
- **Vector/List** - e.g., `[1, 2, 3]`
- **Dict/Record** - e.g., `{"key": "value"}`
- **Packed Float64 Array** - e.g., `bc::f64_array(100)`
- **Tensor** - e.g., `[[1, 2], [3, 4]]`

## Functions

```csy
# Function definition
func add(a, b):
    rt a + b;
::ext

# Function with no return value
func greet(name):
    bc::out>"hello, " + name;
    rt 0;
::ext

# Recursive function
func fib(n):
    if n < 2:
        rt n;
    ::ext
    rt fib(n - 1) + fib(n - 2);
::ext
```

**Key points:**
- Functions start with `func name(params):`
- Function body ends with `::ext`
- Return value with `rt value;`
- Parameters are implicitly typed
- Functions can be recursive
- A call needs exactly as many arguments as the function has parameters. A call the compiler can
  see is a compile error; a call through a function value is a runtime error

## Return Statement

```csy
rt value;  # Return a value
rt 0;      # Return 0 (common for void-like functions)
```

## Conditionals

```csy
# Basic if
if x > 5:
    bc::out>"big";
::ext

# If-else
if x > 5:
    bc::out>"big";
else:
    bc::out>"small";
::ext

# Multiple branches with 'orf' (or-if)
if x < 0:
    rt "negative";
orf x == 0:
    rt "zero";
orf x < 10:
    rt "small";
else:
    rt "big";
::ext
```

**Key points:**
- Conditional starts with `if condition:`
- Alternative branches use `orf condition:` (or-if)
- Default branch uses `else:`
- Block ends with `::ext`

## Loops

### For Loop (Range-based)

```csy
# Iterate from 0 to N-1
for i in thru 5:
    bc::out>i;
::ext

# Iterate over a vector
for x in items:
    bc::out>x;
::ext
```

`for x in value` walks a vector, an f64_array or an i64_array element by element, a string
character by character, and a dict key by key (in insertion order, over a snapshot of the keys, so
removing entries inside the loop is safe). Any other value (an int, a float, `none`) is a runtime
error, and `thru` needs a number.

### While Loop

```csy
i = 0;
while i < 3:
    bc::out>"while-";
    bc::out>i;
    i += 1;
::ext
```

**Key points:**
- Loop body ends with `::ext`
- `thru N` generates range 0 to N-1
- `for var in collection` iterates over elements

## Loop Control

```csy
# skip - continue to next iteration
for i in thru 10:
    if i % 2 == 0:
        skip;  # Skip even numbers
    ::ext
    bc::out>i;
::ext

# stop - break out of loop
for i in thru 10:
    if i == 5:
        stop;  # Stop at 5
    ::ext
    bc::out>i;
::ext
```

## Classes

CSy classes are syntactic sugar - they desugar to constructor functions returning dicts.

```csy
class Dog(name, energy):
    func bark(me):
        rt me::name + " says woof!";
    ::ext
    func play(me):
        if me::energy < 10:
            rt me::name + " is too tired, and naps instead.";
        ::ext
        me::energy -= 10;
        rt me::name + " plays! Energy left: " + bc::str(me::energy);
    ::ext
::ext

# Usage
d = Dog("Rex", 30);
bc::out>d::bark();
```

**Key points:**
- Class defined with `class Name(params):`
- Methods defined inside with `func me_method(me):`
- `me::field` accesses instance fields (desugars to `me["field"]`)
- Body ends with `::ext`
- Classes and dicts are interchangeable under the hood

**Important restrictions:**
- Classes can ONLY contain method definitions (`func ...`)
- Variable declarations (`var x = ...`) are NOT allowed inside class bodies
- Class parameters automatically become instance fields
- Additional fields must be initialized inside methods, not at class level

**Incorrect (will not compile):**
```csy
class Model(name):
    var name = name;        # ERROR: var not allowed in class body
    var weights = bc::f64_array(4);  # ERROR: var not allowed in class body
    func initialize(me):
        me::weights[0] = 0.5;
        rt 0;
    ::ext
::ext
```

**Correct:**
```csy
class Model(name):
    func initialize(me):
        # Initialize additional fields in methods
        me::weights = bc::f64_array(4);
        me::weights[0] = 0.5;
        rt 0;
    ::ext
::ext

# The parameter 'name' is automatically a field
model = Model("DemoNet");
model::initialize();
```

### Inheritance (extends)

A class can extend another class (never itself, directly or through its base classes). Write the base class and the arguments it should
receive after `extends`:

```csy
class Animal(name):
    func speak(me):
        rt me::name + " makes a sound";
    ::ext
    func twice(me):
        rt me::speak() + ", " + me::speak();
    ::ext
::ext

class Dog(name, tricks) extends Animal(name):
    func speak(me):
        rt me::name + " barks";
    ::ext
::ext

d = Dog("Rex", 3);
bc::out>d::speak();    # Rex barks
bc::out>d::twice();    # Rex barks, Rex barks
bc::out>d::tricks;     # 3
```

- Write the base class arguments after its name (`Animal(name)`), using the derived
  class's own parameter names. A wrong number of arguments is a warning.
- The derived class has all the base class methods and fields plus its own.
- A method with the same name replaces the base one, and base methods that call
  `me::speak()` use the replacement.
- Chains work: `class C(...) extends B(...)` where `B` extends `A`.
- One base class only. The base class can be written above or below the derived class.

## Dictionaries/Records

```csy
# Dict literal
tally = {"rested": 0, "tired": 0};

# Access by key
tally["rested"] += 1;

# String keys
person = {"name": "Alice", "age": 30};
bc::out>person["name"];

# With class syntax (me::)
rt me::name;  # Equivalent to me["name"]
```

A dict keeps the order in which its keys were first added. Printing, `bc::keys`, `bc::values`,
`bc::items`, `bc::dict_merge` and JSON output all follow that order. Assigning an existing key keeps
its position; removing a key and adding it again puts it at the end.

```csy
bc::items(tally);              # [["rested", 0], ["tired", 0]]: a vector of [key, value] pairs
bc::remove(tally, "tired");    # removes the key and returns its value (error if the key is missing)
```

## Vector Operations

```csy
# Vector literal
items = [1, 2, 3, 4, 5];

# Indexing (0-based)
bc::out>items[0];  # First element
bc::out>items[4];  # Last element

# Negative indexing (from end)
bc::out>items[-1];  # Last element

# Assignment
items[2] = 99;

# Length
bc::out>bc::len(items);

# Add element
bc::push(items, 100);

# Take out and insert (these change the vector in place)
last = bc::pop(items);           # removes and returns the last element (error if empty)
first = bc::pop(items, 0);       # removes and returns the element at an index
bc::insert(items, 1, 42);        # inserts before an index; the index may equal the length
gone = bc::remove(items, 0);     # removes and returns the element at an index
bc::index_of(items, 42);         # first index of a value, or -1; compares by contents
bc::any([0, 3]);                 # true if any element is truthy
bc::all([1, 0]);                 # true if every element is truthy
bc::zip([1, 2, 3], ["a", "b"]);  # [[1, "a"], [2, "b"]]: stops at the shorter vector
bc::unique([3, 1, 3, 2, 1]);     # [3, 1, 2]: keeps the first of each value, in order
bc::sort_by(words, bc::len);     # a new vector ordered by a key function (stable); keys must be all numbers or all strings
```

A negative index counts from the end for `pop`, `insert` and `remove`.

# Iteration
for x in items:
    bc::out>x;
::ext

# Slice: a new vector holding items[start] up to but not including items[end]
bc::slice(items, 1, 3);    # [2, 3]
bc::slice(items, 2);       # from index 2 to the end
bc::slice(items, -2);      # the last two elements
bc::slice(items, 1, -1);   # everything except the first and last

# Shuffle: a new vector in random order (seeded by bc::seed); the original is untouched
mixed = bc::shuffle(items);

# Copy: an independent copy (see the note below)
backup = bc::copy(items);

# Range: the integers 0 to n - 1 as a vector (takes exactly one number)
order = bc::range(5);              # [0, 1, 2, 3, 4]
picked = bc::shuffle(bc::range(5)); # the same numbers in random order
```

`bc::slice` and `bc::shuffle` also work on an f64_array and return an f64_array. A negative index
counts from the end. A range that falls outside the collection is an error, not a clamp. Strings
are not sliced with `bc::slice`; use `bc::substr`.

Assigning a vector, dict, f64_array or i64_array does not copy it. After `b = a`, both names refer
to the same data, so `b[0] = 5` also changes `a`. `bc::copy(x)` builds an independent copy of
vectors and dicts, and it copies nested vectors and dicts too. `bc::slice` copies only the top level,
so the rows of a nested vector are still shared with the original.

## String Operations

```csy
# String literal
a = "hello";
b = "world";

# Concatenation
c = a + ", " + b + "!";

# Length
bc::out>bc::len(c);

# Built-in string functions
bc::out>bc::upper(s);      # Uppercase
bc::out>bc::lower(s);      # Lowercase
bc::out>bc::substr(s, 7, 5);  # Substring
bc::out>bc::find(s, "World");  # Find substring
bc::out>bc::replace(s, "World", "CSy");  # Replace
bc::out>bc::trim("  padded  ");  # Trim whitespace
bc::out>bc::repeat("ab", 3);  # Repeat string
bc::out>bc::split("a,b,c,d", ",");  # Split into vector
bc::starts_with(s, "Hel");     # true or false
bc::ends_with(s, "ld");
bc::lines(text);               # split at line breaks; a trailing newline adds no empty line, "\r\n" is handled
bc::pad_left("7", 3, "0");     # "007" (the fill is one character and defaults to a space)
bc::pad_right("ab", 5);        # "ab   "
bc::chars("aé€");              # ["a", "é", "€"]: one string per UTF-8 character
bc::ord("€");                  # 8364: the code point of the first character
bc::chr(8364);                 # "€"
```

Strings hold Unicode text and every position counts characters, not bytes: `bc::len("日本語")` is `3`,
`"日本語"[1]` is `"本"`, a negative index counts from the end, `bc::substr`, `bc::find`, `bc::reverse`,
`bc::split(s, "")`, `bc::chars`, `bc::pad_left`, `bc::pad_right` and `for c in s` all work on characters.
Bytes that are not valid UTF-8 count as one character each. String literals understand `\n`, `\t`,
`\\`, `\"`, `\0`, `\xHH` (one byte), `\uHHHH` and `\UHHHHHHHH` (a character); a malformed `\x`, `\u`
or `\U` escape is an error.

### Formatting with bc::format

```csy
bc::format("{} + {} = {}", 1, 2, 3);          # "1 + 2 = 3"
bc::format("pi is {:.3}", 3.14159);           # "pi is 3.142": digits after the point
bc::format("[{:>6}] [{:<6}]", 42, "ab");      # "[    42] [ab    ]": width with alignment
bc::format("{{braces}} {}", "ok");            # "{braces} ok": write {{ and }} for a literal brace
```

Placeholders are filled in order. A placeholder is `{}` or `{:` followed by an optional alignment
(`<` or `>`), an optional width, and an optional `.` with a number of digits. Numbers align right and
other values align left unless you say otherwise. Too few or too many values is an error.
Floats printed without an explicit precision use the shortest decimal representation that round-trips
to the same value.

## Math Operations

### Arithmetic Operators
```csy
x = 10 + 5;   # Addition
y = 10 - 5;   # Subtraction
z = 10 * 5;   # Multiplication
w = 10 / 5;   # Division
i += 1;       # Increment
i -= 1;       # Decrement
i *= 2;       # Multiply in place
i /= 2;       # Divide in place
i %= 5;       # Modulo in place
```

- `%` is modulo. It works on integers and on floats (`7.5 % 2.0` is `1.5`); the result takes the sign of the left operand (`-7 % 3` is `-1`). Modulo by zero is a runtime error. `/` between two integers is integer division and truncates toward zero (`10 / 4` is `2`, `-7 / 2` is `-3`); use a float on either side for a fractional result (`10.0 / 4` is `2.5`).
- Compound assignment exists for `+=`, `-=`, `*=`, `/=` and `%=`. The target is read and written once, so `v[next()] *= 2;` calls `next()` one time.
- Integer arithmetic that overflows 64 bits stops with a runtime error instead of wrapping.
- `+ - * / %` need two numbers. `+` also joins two strings and joins or adds vectors, and an int mixed with
  a float is a float. Anything else (`none + 1`, `1.5 + none`, `1 + "a"`, `"a" * 3`, `-"a"`) is a runtime error
  that names both types. Join text and numbers with `bc::str`.
- Comparing an int with a float is exact: `9007199254740993 == 9007199254740992.0` is `false`.

### Comparison Operators
```csy
x > 5;   # Greater than
x < 5;   # Less than
x == 5;  # Equal
x != 5;  # Not equal
x >= 5;  # Greater or equal
x <= 5;  # Less or equal
```

`<`, `>`, `<=` and `>=` compare two numbers or two strings. Any other pair (a string and an int, `none`,
a vector, a boolean) is a runtime error, so `1 < 2 < 3` stops with an error instead of giving a wrong answer.
`==` and `!=` accept any two values and answer `false` / `true` when the types differ.

### Logical Operators
```csy
a && b;  # Logical AND
a || b;  # Logical OR
!a;      # Logical NOT
```

### Built-in Math Functions
```csy
bc::sqrt(16);       # Square root
bc::pow(2, 10);     # Power
bc::sin(0);         # Sine
bc::cos(0);         # Cosine
bc::log(bc::E());   # Natural log
bc::floor(3.7);     # Floor
bc::ceil(3.2);      # Ceiling
bc::round(3.5);     # Round to a whole number (halves round away from zero)
bc::round(3.14159, 2);  # Round to 2 digits after the point: 3.14
bc::round(1234.5, -2);  # A negative count rounds to tens, hundreds, ...: 1200
bc::abs(-42);       # Absolute value
bc::max(10, 20, 15);  # Maximum of any number of arguments (a NaN is ignored unless all are NaN)
bc::min(10, 20);    # Minimum
bc::max([4, 9, 2]); # Largest element of a vector or f64_array (same for bc::min)
bc::exp(1);         # e raised to a power
bc::isnan(x);       # true if x is NaN (a null loaded from Parquet or CSV is NaN)
bc::isinf(x);       # true if x is +inf or -inf
bc::std(values);    # Standard deviation of a vector or f64_array (divides by n)
bc::var(values);    # Variance (divides by n); bc::var(values, 1) divides by n - 1
bc::clamp(15, 0, 10);  # Clamp to range (NaN if any argument is NaN)
bc::PI();           # Pi constant (a call; bare bc::PI is a function value, not the number)
bc::E();            # Euler's number (same rule)
```

## Built-in Functions

### Output
```csy
bc::out>value;  # Print value to stdout
o>value;        # Same if using bc::out as o
```

### Input
```csy
input = bc::input("prompt> ");  # Read line from stdin, none at end of input
```

### Type Conversion
```csy
bc::str(42);          # Convert to string
bc::int("123");       # Convert to integer; surrounding spaces and a sign are allowed
bc::float("3.5e2");   # Convert to float
bc::typeof(42);       # Get type as string
```

### Random
```csy
bc::seed(42);         # Set random seed
bc::random(-1, 1);    # Random float in range
bc::randint(1, 10);  # Random integer in range
```

### Utility
```csy
bc::len(collection);           # Length of vector/string
bc::sort(nums);                # Sort vector
bc::reverse(nums);             # Reverse vector
bc::contains(nums, 5);          # Check if element exists
bc::contains("hello", "ell");   # Check substring
bc::join(["a", "b", "c"], "-"); # Join vector with separator
```

### File I/O
```csy
bc::read_file(path);       # Read file contents as string
bc::write_file(path, content);  # Write string to file
bc::file_exists(path);    # Check if file exists
```

### Time
```csy
bc::time();       # Current time in seconds
bc::time_ns();    # Current time in nanoseconds
```

### Error Handling
```csy
bc::raise("error message");  # Raise an error
```

### Exit
```csy
bc::exit(0);   # Exit with code
bc::exit(1);   # Exit with error code
```

### Command-line Arguments
```csy
#inc basic as bc;

arguments = bc::args();
for argument in arguments:
    bc::out>argument;
::ext
```

`bc::args()` returns every program argument as a string, in order. With a source file, put
arguments after `--` so `csyc` does not interpret them as compiler options or project files:

```sh
csyc script.csy -- input.txt --verbose
```

The program receives `["input.txt", "--verbose"]`; the separator itself is not included. The same
separator works when running a compiled object:

```sh
csyc run program.cso -- input.txt --verbose
```

For a standalone executable built with `csyc build script.csy --exe -o program`, pass arguments
after the executable name:

```sh
./program input.txt --verbose
```

### Experimental native-only executable

On Linux x86-64, a deliberately small subset can be compiled to a native executable:

```csy
func main():
    rt 6 * 7;
::ext
```

```sh
csyc build main.csy --native-exe -o program
./program
echo $?
```

The only currently supported source is one zero-argument `main` function whose sole statement
returns an integer expression formed from integer literals, unary minus, and `+`, `-`, or `*`.
The process exit status is the low 8 bits of the returned value. Other CSy features are rejected;
this mode never falls back to bytecode or the VM. The build uses the host C compiler (`CC`, or
`cc` when unset), but the resulting executable does not need `csyc` or a CSy runtime to run.
This prototype requires `--native-exe` together with `build` and `-o`, and cannot be combined
with `--exe`, `--no-native`, or program arguments.

CSy returns the arguments unchanged. It does not automatically interpret flags, convert values,
or validate required arguments. Without `--`, extra words passed to `csyc` are treated as compiler
inputs/options, not program arguments.

## Tensor Operations

```csy
# Creation
A = bc::ones(2, 3);      # Matrix of ones
I = bc::eye(3);          # Identity matrix
Z = bc::zeros(2, 2);     # Matrix of zeros

# Manual tensor
B = [
    [1, 2, 3],
    [4, 5, 6]
];

# Matrix operations
R = bc::matmul(C, D);    # Matrix multiplication (works, but warns: deprecated, prefer ai::matmul)
T = bc::transpose(B);   # Transpose

# Vector operations
bc::dot(v1, v2);         # Dot product
bc::sum(v1);             # Sum of elements
bc::mean(v1);            # Mean of elements

# Shape
bc::shape(B);            # Get tensor shape

# Print tensor
bc::tprint(A);           # Pretty-print tensor
```

## Packed Float64 Arrays

For high-performance numeric workloads, CSy provides packed Float64 arrays.

```csy
# Create packed array
weights = bc::f64_array(64);
weights[3] = 0.125;
weights[4] += 0.25;

# Elementwise operations with scalars
result = weights + 0.5;
result = weights * 2.0;

# Elementwise operations with arrays (same length)
result = weights * other_weights;

# Save/load checkpoints
bc::save_f64("model.csyf64", weights);
loaded = bc::load_f64("model.csyf64");

# Negative indexing
bc::out>weights[-1];  # Last element
```

**Key points:**
- Fixed-size, contiguous, mutable Float64 buffer
- Elements stored as raw doubles (no tagged Value per element)
- Supports elementwise +, -, *, / with scalars or equal-length arrays
- Used for AI model parameters
- Can be compiled to native code for performance

### Checkpoint files (.csyf64)

`bc::save_f64(path, array)` writes a binary file and returns `true`; `bc::load_f64(path)` reads it
back as an `f64_array`. The layout is an 8-byte magic header, the value count (8 bytes,
little-endian), the raw little-endian doubles, and an 8-byte xxHash64 checksum. An array of 1000
values takes 8,024 bytes.

- Saves are atomic. The data goes to a temporary file that is renamed into place, so a crash or a
  kill in the middle of a save leaves the previous checkpoint intact and no partial file behind.
- `bc::load_f64` checks the header, the file size and the checksum before it returns, and the size
  is checked before any memory is allocated. A truncated, extended or corrupted file raises an
  error instead of loading wrong numbers.
- NaN, infinity, negative zero and subnormal values round trip bit for bit.
- The limit is 134,217,728 values (see Limits and Caps).
- Old text checkpoints (first line `CSYF64 1`) still load, up to 16 million values or 128 MiB.
  Saving the loaded array again converts it to the binary format. Text checkpoints cannot hold NaN
  or infinity.
- The file stores values only, not a shape or a scaler. Keep the layer sizes and any
  normalization statistics next to it, for example in a JSON file (see Training a Model from a
  Parquet File).

On a 4 million value array the old text format took about 1.8 s to save and 1.0 s to load; the
binary format takes about 0.04 s and 0.03 s.

### Which builtins accept an f64_array

Elementwise math returns a new `f64_array`, so the result can go straight into `ai::` functions.

| Builtin | With an f64_array | Result |
|---|---|---|
| `bc::len`, `bc::sum`, `bc::mean`, `bc::has(a, x)`, `bc::contains(a, x)` | works | int, float, float, bool, bool |
| `bc::max(a)`, `bc::min(a)` | works | float |
| `bc::std(a)`, `bc::var(a)` | works (divides by n; `bc::var(a, 1)` divides by n - 1) | float |
| `bc::slice(a, start, end)`, `bc::copy(a)`, `bc::shuffle(a)` | works | new f64_array |
| `bc::sqrt`, `exp`, `log`, `log2`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `abs`, `floor`, `ceil`, `round`, `sign`, `sigmoid`, `relu`, `tanh` and the `_deriv` functions | applied to every element | new f64_array |
| `bc::softmax(a)`, `bc::softmax(a, cols)` | works | new f64_array |
| `bc::argmax(a)`, `bc::argmax(a, cols)` | works | int, or a vector of ints with `cols` |
| `bc::sort(a)`, `bc::reverse(a)` | works (NaN sorts last) | new f64_array |
| `bc::shape(a)` | works | `[length]` |
| `bc::dot(a, b)` | works with f64_arrays or vectors of equal length | float |
| `bc::join(a, sep)` | works | string |
| `a + b`, `a * 2.0`, `a - b`, `a / b` | elementwise | new f64_array |
| `bc::push(a, x)` | error: an f64_array has a fixed size | |
| `bc::map`, `bc::filter`, `bc::reduce`, `bc::transpose`, `bc::sum_cols` | error: they need a vector or a nested vector | |

Passing something that is not numeric (a string, a dict, `none`) to a math function is an error;
it is never treated as 0. Convert a vector with `bc::to_f64(vector)`.

A network output is one flat buffer of `rows * cols` values. Pass `cols` to work row by row:

```csy
probs = bc::softmax(logits, 3);   # f64_array; every group of 3 values sums to 1
picks = bc::argmax(probs, 3);     # vector with one class index per row
```

Without `cols`, `bc::softmax` treats the whole array as a single row. `cols` must be a positive
integer that divides the length of the array.

## Packed Int64 Arrays

Use `bc::i64_array(length[, initial_value])` for a fixed-size, mutable array of signed 64-bit
integers. The initial value defaults to zero. Indexing accepts negative indices, and values remain
exact across the full signed 64-bit range.

```csy
values = bc::i64_array(3);
values[0] = 9007199254740993;
values[1] = 4;
values[2] = 5;

func sum_values(items):
    total = 0;
    for i in thru bc::len(items):
        total += items[i];
    ::ext
    rt total;
::ext

bc::out>sum_values(values);
```

Native numeric functions can read, write and update an `i64_array` parameter. Like `f64_array`,
it has a fixed size, so `bc::push` cannot grow it. Assignment shares the same array, so changing
one variable changes the other.

## Datasets and JSON

CSy can read and write AI datasets as JSON, JSONL, Parquet, CSV, `.bin` tensors and plain
text files. All of these are in `bc`. Every save writes to a temporary file first and
renames it when finished, so a failed save never leaves a half written dataset. Errors
are ordinary catchable errors with the file name and, for text formats, the line and
column.

### JSON

```csy
v = bc::json_parse("{\"a\": [1, 2.5, true, null]}");
text = bc::json_stringify(v);
pretty = bc::json_stringify(v, true);
data = bc::load_json("config.json");
bc::save_json("out.json", data, true);
```

- Objects become dicts, arrays become vectors, `null` becomes `none`.
- A number without a fraction or exponent that fits in 64 bits is an int, any other number is a float.
- The parser is strict: no trailing commas, no comments, no leading zeros, strings must be valid UTF-8.
  The one extension is that `NaN`, `Infinity` and `-Infinity` are accepted, because Python writes them.
- Nesting deeper than 256 levels is refused.
- Dict keys are written in sorted order, so the same data always gives the same file.
- `NaN` and infinity are written as `null`, because JSON cannot hold them.
  Floats always keep a `.0` or exponent, so a float reads back as a float.

### JSONL

```csy
rows = bc::load_jsonl("train.jsonl");
first_100 = bc::load_jsonl("train.jsonl", 100);
bc::save_jsonl("copy.jsonl", rows);
```

One JSON value per line. Blank lines are skipped, `\r\n` line endings and a leading
byte order mark are accepted. A bad line is reported with its line number.

### Parquet

```csy
t = bc::load_parquet("data.parquet");
x = t["columns"]["height"];
names = t["names"];
rows = t["rows"];

some = bc::load_parquet("data.parquet", ["height", "label"]);
head = bc::load_parquet("data.parquet", none, 1000);

bc::save_parquet("out.parquet", t);
bc::save_parquet("out2.parquet", {"a": [1, 2, 3], "b": ["x", "y", "z"]});
```

`load_parquet` returns a dict with `names` (column names in file order), `rows`, and
`columns` (a dict from name to column). Float and double columns come back as
`f64_array` so they can go straight into `ai::` functions. Integer, bool and string
columns come back as vectors. A null is `none` in a vector and `NaN` in an `f64_array`.
Pass a vector of names to load only those columns, and a number as the third argument
to stop after the row group that reaches that many rows (the result is cut to exactly
that many).

`save_parquet` takes the same table shape, or a plain dict of columns (written in
sorted name order). A column is an `f64_array`, or a vector of one kind of value:
numbers, bools or strings, with `none` for missing values. Ints mixed with floats
become doubles. Files are written uncompressed, one row group per 1048576 rows.

What it reads: flat tables, UNCOMPRESSED, SNAPPY, GZIP and ZSTD, PLAIN, dictionary,
RLE and BYTE_STREAM_SPLIT encodings, data page versions 1 and 2. Dates, timestamps and
decimals come back as their raw numbers. Struct fields come back as columns named by their path
(`person.age`, null where the struct or the field is null). A list column comes back as one vector
per row: `none` for a null list, an empty vector for an empty list, and `none` for a null element.
Not supported, with a clear error: lists inside lists, LZ4 and Brotli,
DELTA encodings, INT96 and fixed length byte columns, and encrypted files.

A file that uses an unsupported codec or encoding can be rewritten once with a supported one, for
example in Python: `pyarrow.parquet.write_table(table, "out.parquet", compression="snappy")`
(pandas: `df.to_parquet("out.parquet", compression="snappy")`).

ZSTD is decoded by CSy's own decoder, with no external library. It reads every compression
level, checks the content checksum when the file has one, and rejects a corrupt page with an
error instead of returning wrong values. A page that needs a ZSTD dictionary is rejected, because
Parquet writers do not use them. `save_parquet` still writes uncompressed files. Reading ZSTD
adds little: on a 2 million row file the decode cost was about 0.4 s, and the rest of the load time
is the same as for an uncompressed file.

### CSV

```csy
t = bc::load_csv("data.csv");
t["names"];
t["rows"];
x = t["columns"]["height"];
bc::save_csv("out.csv", t);
bc::save_csv("out2.csv", {"a": [1, 2, 3], "b": ["x", "y", "z"]});
```

`load_csv` returns the same table shape as `load_parquet`: a dict with `names`, `rows` and
`columns`. The first line is the header. Each column gets one type, decided from every row of
the file: all bools (`true` or `false`, any case) give a vector of bools, all whole numbers that fit
in 64 bits give a vector of ints, any other column of numbers (including `nan` and `inf`) gives an
`f64_array` that can go straight into `ai::` functions, and anything else gives a vector of strings.
An empty cell is `none` in a bool or int vector, `NaN` in an `f64_array`, and an empty string in a
string vector.

Options go in a dict as the second argument: `delimiter` (one character, for example `";"`;
`.tsv` files default to a tab), `header` (`false` if the file has no header line, the columns are then
called `col0`, `col1`, ...), `columns` (a vector of names to load), `types` (a dict from column name
to `"int"`, `"f64"`, `"bool"` or `"str"`, to override the guess) and `max_rows`.

Quoted fields, doubled quotes, line breaks inside quotes, `\r\n` line endings and a leading byte
order mark are all handled. A row with the wrong number of fields is an error that names its
line number, and so is a cell that does not fit its column's type.

`save_csv` takes a table, or a plain dict of columns (written in sorted name order). `NaN` and `none`
are written as an empty cell. A string such as `"007"` is written as it is and will read back as the
number 7 unless you pass `types`.

### .bin tensors

```csy
bc::save_bin("images.bin", [[1, 2, 3], [4, 5, 6]]);
r = bc::load_bin("images.bin");
r["shape"];
r["data"];

bc::save_bin("w.bin", weights, [2, 3]);
bc::save_bin("w32.bin", weights, [2, 3], "f32");
```

`save_bin(path, data[, shape][, dtype])` writes a self describing file: dtype, shape,
the values, and a checksum. `data` is an `f64_array` or a rectangular nested vector of
numbers. The dtype is `f64`, `f32`, `i64`, `i32` or `u8`; without one, a vector of ints is
stored as `i64` and anything else as `f64`. `load_bin` returns a dict with `dtype`,
`shape` and `data`, where `data` is a flat row major `f64_array`. A damaged or truncated
file is refused. An `i64` value that cannot be kept exactly as a double is an error.

For files with no header, such as arrays written by NumPy `tofile`:

```csy
x = bc::load_bin_raw("features.bin", "f32");
y = bc::load_bin_raw("with_header.bin", "u8", 16);
bc::save_bin_raw("out.bin", x, "f32");
```

All values are little endian.

### Text files and one call for everything

```csy
lines = bc::load_text("corpus.txt");
bc::save_text("copy.txt", lines);

data = bc::load_dataset("train.jsonl");
bc::save_dataset("train.parquet", table);
```

`load_text` returns one string per line. `load_dataset` and `save_dataset` pick the
format from the extension: `.parquet` or `.pq`, `.csv` or `.tsv`, `.jsonl` or `.ndjson`, `.json`, `.bin`,
and anything else is a plain text file. Extra arguments are passed on to the matching
function.

`bc::to_f64(values)` turns a rectangular nested vector of numbers into one flat
`f64_array`, for example the feature lists you get from JSON.

The functions above load a whole file, so they are limited to files up to 1 GiB and
tables up to 134217728 values. For anything bigger, read and write in batches.

### Big datasets: reading and writing in batches

```csy
h = bc::open_dataset("train.parquet", {"columns": ["x", "label"]});
total = 0;
while true:
    batch = bc::next_batch(h, 4096);
    if batch == none:
        stop;
    ::ext
    total += batch["rows"];
::ext
bc::close_dataset(h);
```

`open_dataset(path[, options])` returns a handle. `next_batch(handle, rows)` returns the
next batch of at most that many rows, and `none` when the data is finished. Only one
batch (plus one Parquet row group) is in memory at a time, so a 1.3 GB Parquet file or
a 1.2 GB JSONL file streams in about 100 MB. `reset_dataset(handle)` starts again from
the beginning (for the next epoch), `dataset_info(handle)` describes the dataset, and
`close_dataset(handle)` releases it. At most 64 datasets can be open at once.

What a batch looks like depends on the format:

- `.parquet`: a table, the same shape as `load_parquet` returns. Option `columns`.
- `.csv` and `.tsv`: a table with up to that many rows. Options `delimiter`, `header`, `columns`, `types`.
  The column types are decided from the first 10000 rows and then kept; a later cell that does not fit
  is an error that names the row and column, and `types` lets you choose the type yourself.
- `.jsonl`: a vector of rows. `.txt` and other files: a vector of lines.
- `.bin`: a dict with `dtype`, `shape` (first number is the rows in this batch) and a flat `data` f64_array.
  For a file without a header pass `{"raw": "f32", "row_len": 784}`, and optionally `"offset"` to skip a header.
  The checksum of a `.bin` file is checked when you read through to the end.

Writing works the same way, and the file only appears when you close it:

```csy
out = bc::create_dataset("features.parquet");
bc::write_batch(out, {"names": ["x"], "columns": {"x": values}});
bc::close_dataset(out);
```

`create_dataset(path[, options])` picks the format from the extension, `write_batch(handle, batch)`
takes the same kind of value that `save_*` takes, and `close_dataset` finishes the file. A
writer that is never closed saves nothing and prints a warning when the program ends.

- Parquet: the first batch fixes the column names and types, later batches must match
  (ints may be added to a float column). Every column is nullable. A batch that does not
  fit is rejected whole, and you can carry on writing. Batches are collected into row groups
  of up to 1048576 rows.
- `.bin`: the first batch fixes the dtype (option `dtype`) and every dimension after the first.
- `.csv` and `.tsv`: the first batch fixes the header, later batches must have the same columns (option `delimiter`).
- `.jsonl` and text files: append rows or lines.
- A `.json` file is a single value, so it cannot be streamed.

`bc::sum` and `bc::mean` also accept an f64_array.

## AI Operations

### Dense Layer
```csy
#inc ai as ai;

# X: input [batch * input_width]
# W: weights [input_width * output_width]
# b: bias [output_width]
Y = ai::dense(X, W, b, batch, input_width, output_width, "relu");
```

Activations: `"linear"`, `"relu"`, `"sigmoid"`, `"tanh"`

### Matrix Multiplication
```csy
# Packed row-major matmul
Z = ai::matmul(a, b, rows, inner, cols);
```

### Backward Pass
```csy
# Returns [dW, db, dInput]
gradients = ai::dense_backward(
    X, W, output, error, batch, input_width, output_width, "sigmoid");
```

`error` is the gradient of the loss with respect to this layer's output. The function applies the
derivative of the activation itself, using `output`, so pass `"linear"` for a layer with no
activation. `dInput` (the third element) is the error to hand to the layer below, which makes a
multi-layer network a chain of calls. Gradients are summed over the batch, so divide by `batch`
when you build `error`.

Training a three-class classifier with softmax and cross-entropy needs this manual loop, because
`ai::mlp_train` only does mean squared error with one activation for both layers. For softmax with
cross-entropy the error at the logits is `(probability - one_hot_target) / rows`:

```csy
#inc basic as bc;
#inc ai as ai;
using bc::out as o;
bc::seed(7);

rows = 300;
width = 2;
classes = 3;
hidden = 8;

X = bc::f64_array(rows * width);
labels = [];
for i in thru rows:
    a = bc::random() * 2.0 - 1.0;
    X[i * width] = a;
    X[i * width + 1] = bc::random() * 2.0 - 1.0;
    c = 1;
    if a < -0.33:
        c = 0;
    orf a > 0.33:
        c = 2;
    ::ext
    bc::push(labels, c);
::ext

W1 = bc::to_f64(bc::rand_mat(width, hidden, -0.5, 0.5));
b1 = bc::f64_array(hidden);
W2 = bc::to_f64(bc::rand_mat(hidden, classes, -0.5, 0.5));
b2 = bc::f64_array(classes);
dZ = bc::f64_array(rows * classes);

for step in thru 301:
    H = ai::dense(X, W1, b1, rows, width, hidden, "relu");
    Z = ai::dense(H, W2, b2, rows, hidden, classes, "linear");
    P = bc::softmax(Z, classes);

    for i in thru rows:
        for c in thru classes:
            target = 0.0;
            if c == labels[i]:
                target = 1.0;
            ::ext
            dZ[i * classes + c] = (P[i * classes + c] - target) / rows;
        ::ext
    ::ext

    g2 = ai::dense_backward(H, W2, Z, dZ, rows, hidden, classes, "linear");
    g1 = ai::dense_backward(X, W1, H, g2[2], rows, width, hidden, "relu");
    ai::sgd_step(W2, g2[0], 0.5);
    ai::sgd_step(b2, g2[1], 0.5);
    ai::sgd_step(W1, g1[0], 0.5);
    ai::sgd_step(b1, g1[1], 0.5);

    if step % 100 == 0:
        picks = bc::argmax(P, classes);
        right = 0;
        for i in thru rows:
            if picks[i] == labels[i]:
                right += 1;
            ::ext
        ::ext
        o>"step " + bc::str(step) + "  accuracy " + bc::str(right * 1.0 / rows);
    ::ext
::ext
```

### SGD Step
```csy
# Update parameters in place
ai::sgd_step(W, dW, learning_rate);
ai::sgd_step(b, db, learning_rate);
```

### Fused MLP Training
```csy
# Full-batch two-layer MLP training in one operation
loss = ai::mlp_train(X, Y, W1, b1, W2, b2,
                     batch, input_width, hidden_width, output_width,
                     epochs, learning_rate, "sigmoid");

# Same, with the gradient averaged over the batch
loss = ai::mlp_train(X, Y, W1, b1, W2, b2,
                     batch, input_width, hidden_width, output_width,
                     epochs, learning_rate, "sigmoid", "mean");
```

By default the gradient is the sum over all `batch` rows, not the average, so the step size grows
with the number of rows. A learning rate that works for 4 rows can overshoot and saturate the
network for 2000 rows. Divide the learning rate by `batch`, or pass `"mean"` as the optional last
argument, which does exactly that. `ai::dense_backward` and `ai::sgd_step` also work with summed
gradients, so scale the learning rate yourself when you use them.

Both layers use the same activation and the loss is always mean squared error, so use it for
regression or for a sigmoid classifier. For softmax with cross-entropy, or a ReLU hidden layer with a
linear output, write the loop with `ai::dense` and `ai::dense_backward` (see Backward Pass).

The returned loss is `0.5 * (sum of squared errors) / batch`, measured with the weights from the start
of the last epoch. If the final loss is not lower than the loss of the first epoch (or is not a number),
`mlp_train` prints a warning on stderr that suggests a smaller learning rate.

### GPU Training
```csy
# Same as mlp_train (summed gradients, no reduction argument) but uses CUDA/cuBLAS
loss = ai::mlp_train_gpu(X, Y, W1, b1, W2, b2,
                         batch, input_width, hidden_width, output_width,
                         epochs, learning_rate, "sigmoid");
```

## Training a Model from a Parquet File

This is a complete, runnable example: a two-layer network that classifies rows of a Parquet file
with 30 numeric feature columns and a `label` column of 0 and 1. It splits the rows, standardizes
the features with statistics from the training rows only, trains, reports accuracy on the held-out
rows, and saves everything a separate program needs to make predictions.

The file is read with `bc::load_parquet`, which returns a table with `"names"`, `"rows"` and
`"columns"`. A numeric column arrives as an `f64_array`. The matrix `ai::` functions expect is one
flat row-major `f64_array`, so the feature columns are copied into it. Every helper that loops over
arrays is a function, which lets it run natively. Run `csyc train.csy --explain-native` to check.

The split here takes the last 20 percent of the rows as the test set, so it assumes the rows are
already in random order. If they are not, build `order = bc::shuffle(bc::range(rows))` and read
each column through it.

### Training program (train.csy)

```csy
#inc basic as bc;
#inc ai as ai;
using bc::out as o;

func fill_rows(X, col, j, start, count, width):
    for i in thru count:
        X[i * width + j] = col[start + i];
    ::ext
    rt 0;
::ext

func fit_scaler(X, rows, width, mu, sd):
    for j in thru width:
        s = 0.0;
        for i in thru rows:
            s += X[i * width + j];
        ::ext
        m = s / rows;
        v = 0.0;
        for i in thru rows:
            d = X[i * width + j] - m;
            v += d * d;
        ::ext
        dev = sqrt(v / rows);
        if dev == 0.0:
            dev = 1.0;
        ::ext
        mu[j] = m;
        sd[j] = dev;
    ::ext
    rt 0;
::ext

func apply_scaler(X, rows, width, mu, sd):
    for i in thru rows:
        for j in thru width:
            X[i * width + j] = (X[i * width + j] - mu[j]) / sd[j];
        ::ext
    ::ext
    rt 0;
::ext

func count_correct(P, Y, rows):
    right = 0;
    for i in thru rows:
        if P[i] > 0.5:
            if Y[i] == 1.0:
                right += 1;
            ::ext
        else:
            if Y[i] == 0.0:
                right += 1;
            ::ext
        ::ext
    ::ext
    rt right;
::ext

bc::seed(42);
t = bc::load_parquet("data.parquet");
rows = t["rows"];
width = bc::len(t["names"]) - 1;
ntr = rows * 8 / 10;
nte = rows - ntr;

Xtr = bc::f64_array(ntr * width);
Xte = bc::f64_array(nte * width);
Ytr = bc::f64_array(ntr);
Yte = bc::f64_array(nte);
for j in thru width:
    col = t["columns"][t["names"][j]];
    fill_rows(Xtr, col, j, 0, ntr, width);
    fill_rows(Xte, col, j, ntr, nte, width);
::ext
label = t["columns"]["label"];
fill_rows(Ytr, label, 0, 0, ntr, 1);
fill_rows(Yte, label, 0, ntr, nte, 1);

mu = bc::f64_array(width);
sd = bc::f64_array(width);
fit_scaler(Xtr, ntr, width, mu, sd);
apply_scaler(Xtr, ntr, width, mu, sd);
apply_scaler(Xte, nte, width, mu, sd);

hidden = 16;
W1 = bc::to_f64(bc::rand_mat(width, hidden, -0.3, 0.3));
b1 = bc::f64_array(hidden);
W2 = bc::to_f64(bc::rand_mat(hidden, 1, -0.3, 0.3));
b2 = bc::f64_array(1);

for round in thru 5:
    loss = ai::mlp_train(Xtr, Ytr, W1, b1, W2, b2, ntr, width, hidden, 1, 100, 0.5, "sigmoid", "mean");
    H = ai::dense(Xte, W1, b1, nte, width, hidden, "sigmoid");
    P = ai::dense(H, W2, b2, nte, hidden, 1, "sigmoid");
    o>"epochs " + bc::str((round + 1) * 100) + "  loss " + bc::str(loss)
       + "  test accuracy " + bc::str(count_correct(P, Yte, nte) * 1.0 / nte);
::ext

bc::save_f64("model_w1.csyf64", W1);
bc::save_f64("model_b1.csyf64", b1);
bc::save_f64("model_w2.csyf64", W2);
bc::save_f64("model_b2.csyf64", b2);
bc::save_f64("model_mu.csyf64", mu);
bc::save_f64("model_sd.csyf64", sd);
bc::save_json("model.json", {"width": width, "hidden": hidden});
```

Output looks like this (the exact numbers depend on your data and seed):

```
epochs 100  loss 0.0256507  test accuracy 0.973684
epochs 200  loss 0.0163851  test accuracy 0.982456
```

### Inference program (predict.csy)

A separate program loads the six checkpoint files and the JSON, applies the saved scaler, and
runs the forward pass.

```csy
#inc basic as bc;
#inc ai as ai;
using bc::out as o;

func fill_rows(X, col, j, start, count, width):
    for i in thru count:
        X[i * width + j] = col[start + i];
    ::ext
    rt 0;
::ext

func apply_scaler(X, rows, width, mu, sd):
    for i in thru rows:
        for j in thru width:
            X[i * width + j] = (X[i * width + j] - mu[j]) / sd[j];
        ::ext
    ::ext
    rt 0;
::ext

cfg = bc::load_json("model.json");
width = cfg["width"];
hidden = cfg["hidden"];
W1 = bc::load_f64("model_w1.csyf64");
b1 = bc::load_f64("model_b1.csyf64");
W2 = bc::load_f64("model_w2.csyf64");
b2 = bc::load_f64("model_b2.csyf64");
mu = bc::load_f64("model_mu.csyf64");
sd = bc::load_f64("model_sd.csyf64");

t = bc::load_parquet("data.parquet");
rows = t["rows"];
X = bc::f64_array(rows * width);
for j in thru width:
    fill_rows(X, t["columns"][t["names"][j]], j, 0, rows, width);
::ext
apply_scaler(X, rows, width, mu, sd);

H = ai::dense(X, W1, b1, rows, width, hidden, "sigmoid");
P = ai::dense(H, W2, b2, rows, hidden, 1, "sigmoid");
o>"first three probabilities:";
for i in thru 3:
    o>P[i];
::ext
```

Two things are easy to forget. The scaler must be saved with the weights, because a network trained
on standardized features gives meaningless output for raw ones. And `save_f64` stores only values,
so keep the layer sizes in the JSON file.

### Mini-batches

`ai::mlp_train` runs full-batch gradient descent. For mini-batches, call it once per slice with
`epochs` set to 1; the weights are updated in place. `bc::slice` cuts a batch out of the flat
feature buffer, and shuffling the order of the batches each epoch is one call. Replace the
`for round` loop of the training program with:

```csy
batch = 64;
nbatches = (ntr + batch - 1) / batch;
for epoch in thru 100:
    for k in bc::shuffle(bc::range(nbatches)):
        first = k * batch;
        last = bc::min(first + batch, ntr);
        Xb = bc::slice(Xtr, first * width, last * width);
        Yb = bc::slice(Ytr, first, last);
        ai::mlp_train(Xb, Yb, W1, b1, W2, b2, last - first, width, hidden, 1, 1, 0.5, "sigmoid", "mean");
    ::ext
    if (epoch + 1) % 25 == 0:
        H = ai::dense(Xte, W1, b1, nte, width, hidden, "sigmoid");
        P = ai::dense(H, W2, b2, nte, hidden, 1, "sigmoid");
        o>"epoch " + bc::str(epoch + 1) + "  test accuracy " + bc::str(count_correct(P, Yte, nte) * 1.0 / nte);
    ::ext
::ext
```

The last batch can be smaller than the others, which is why `last - first` is passed as the number of
rows. This shuffles the order of whole batches; if the rows are sorted by class, shuffle them once
before splitting.

### Recipes for things that are not builtins

CSy has no builtin for `concat`, `randn`, `one_hot`, `cross_entropy`, `argsort`, `unique`, `cumsum`,
`adam` or `dropout`. These are small functions you can write yourself. Loops over arrays and numbers run natively when they sit inside a function.

Shuffled row indices: `bc::shuffle(bc::range(rows))` returns the numbers 0 to `rows - 1` in random
order, and `bc::seed` makes it repeatable.

One-hot encoding of integer class labels into a flat `rows * classes` buffer:

```csy
func one_hot(labels, rows, classes):
    Y = bc::f64_array(rows * classes);
    for i in thru rows:
        Y[i * classes + labels[i]] = 1.0;
    ::ext
    rt Y;
::ext
```

Counting missing values (nulls load as NaN). A NaN that reaches `ai::dense` or `bc::mean` turns every
result into NaN, so fill or drop missing values before training:

```csy
func count_nan(X, n):
    c = 0;
    for i in thru n:
        if isnan(X[i]):
            c += 1;
        ::ext
    ::ext
    rt c;
::ext
```

Filling missing values in one column of a flat feature matrix with that column's mean:

```csy
func fill_nan_mean(X, rows, width):
    for j in thru width:
        s = 0.0;
        n = 0;
        for i in thru rows:
            if isnan(X[i * width + j]):
                skip;
            ::ext
            s += X[i * width + j];
            n += 1;
        ::ext
        m = 0.0;
        if n > 0:
            m = s / n;
        ::ext
        for i in thru rows:
            if isnan(X[i * width + j]):
                X[i * width + j] = m;
            ::ext
        ::ext
    ::ext
    rt 0;
::ext
```

Adam, with the first and second moment buffers `m` and `v` (f64_arrays the same size as the
parameter) and a step counter that starts at 1. Pass the length `n` in; `bc::len(W)` inside the
function would keep it interpreted:

```csy
func adam(W, g, m, v, lr, step, n):
    beta1 = 0.9;
    beta2 = 0.999;
    c1 = 1.0 - pow(beta1, step);
    c2 = 1.0 - pow(beta2, step);
    for k in thru n:
        m[k] = beta1 * m[k] + (1.0 - beta1) * g[k];
        v[k] = beta2 * v[k] + (1.0 - beta2) * g[k] * g[k];
        W[k] -= lr * (m[k] / c1) / (sqrt(v[k] / c2) + 0.00000001);
    ::ext
    rt 0;
::ext
```

Call it once per parameter tensor after `ai::dense_backward`, for example
`adam(W1, g1[0], mW1, vW1, 0.01, step, bc::len(W1));`.

`bc::std` and `bc::var` work on one vector or f64_array. The per-column mean and standard
deviation of a flat feature matrix are computed in the training program above (`fit_scaler`).


## Projects and Modules

A project is several `.csy` files built into one `.cso`. This section is the whole
guide: the files, the `project.csp` that describes them, `#connect`, and the
commands.

### A project, step by step

Start with `csyc init`, which makes the folder, a `project.csp`, an entry file and a copy of the Cooler Compiler table:

```bash
csyc init myapp
cd myapp
```

Put your other files anywhere under it. This example uses a `lib` folder:

```text
myapp/
  project.csp
  src/
    main.csy          the entry file
    lib/
      math.csy
      report.csy
```

`src/lib/math.csy` - a file that shares a variable and two functions:

```csy
answer = 42;

func double(x):
    rt x * 2;
::ext

func _helper(x):          # starts with '_', so it is private to this file
    rt x * 10;
::ext

func triple(x):
    rt _helper(x) / 10 * 3;
::ext
```

`src/lib/report.csy` - a file that connects another file next to it:

```csy
#inc basic as bc;
#connect "math.csy";      # relative to THIS file, so it finds src/lib/math.csy

func show(x):
    bc::out > math::double(x);
::ext
```

`src/main.csy` - the entry file:

```csy
#inc basic as bc;
#connect "lib/math.csy";
#connect "lib/report.csy";

bc::out > math::answer;
report::show(21);
bc::out > math::triple(5);
```

`project.csp` - the project file:

```toml
entry = "src/main.csy"
sources = []
output = "build/app.cso"
```

Build and run it:

```bash
csyc build                  # reads ./project.csp, writes build/app.cso
csyc run build/app.cso      # prints 42, 42, 15
csyc project.csp            # shorthand: build and run in one go
```

You never list `lib/math.csy` or `lib/report.csy` in the project file: the compiler
follows every `#connect`, loads each file once, checks the names, and links the
result. `math.csy` is connected by two files here and still exists only once.

### The project file (.csp)

```toml
entry = "src/main.csy"       # the file that starts the program (required)
sources = []                 # optional extra files (see below)
output = "build/app.cso"     # optional; without it the .cso is written next to the entry file
```

- `entry` and `sources` are paths relative to the `.csp` file.
- Files that are connected with `#connect` are found automatically and do not
  belong in `sources`. A file listed in `sources` only has its top-level code run
  before the entry file; its names are still not visible without `#connect`.
- `csyc build file.csy -o out.cso` builds one file without a project file, and
  `csyc build project.csp -o out.cso` overrides `output`. The output may not be a source file of the
  build, may not end in `.csy`, and may only replace a missing, empty or earlier build output (a CSy object or an
  executable); a device, a folder or an unrelated file is refused.
- A `.cso` is the result, never an input: `#connect` cannot point at one.

### Connecting files

```csy
#connect "math.csy";                       # namespace "math" (the file name)
#connect "scripts/math.csy";               # a sub folder, namespace still "math"
#connect "../shared/tools.csy" as t;       # your own namespace name
#connect "math.csy" as m { double; answer; }   # only these names (an allowlist)
```

- The namespace defaults to the file name without its folders and extension. If
  that is not a usable name (`my-math.csy`, `2d.csy`), write `as name`.
- Without braces, everything the file declares at its top level is shared: its
  functions and its variables. With braces, only the listed names are.
- A name that starts with `_` is private. `#connect` never shares it, and listing
  it in an allowlist is an error.
- Use shared names as `namespace::name`, for functions and variables alike:
  `math::double(4)`, `math::answer`, and `math::answer = 7;` to assign.
- Bare names from a connected file are not visible; write `math::name`.
- Paths are relative to the file that contains the `#connect`.

**Key points:**
- A file is loaded once, however many files connect it; its top-level code runs
  once, before the code of the files that connect it.
- A namespace belongs to the file that declares it, so two files can both use `u`
  for different modules. In one file a namespace names one module and cannot reuse
  an `#inc` alias such as `bc`.
- Functions that write module variables stay interpreted (no native code for them).

### Errors you may meet

| Message (shortened) | Cause | Fix |
|---|---|---|
| `unknown name 'double'` | used a connected name without its namespace | write `math::double` |
| `'dubel' is not exposed by connection 'math'` | no such shared name; the note lists close matches | fix the spelling, or remove the `_` if it is private |
| `'_helper' is private in ...` | an allowlist names a private function | rename it without the underscore |
| `#connect expects a .csy source file` | the path is not a `.csy` file (a `.cso` is a build output) | connect the `.csy` it was built from |
| `cannot be used as a namespace name` | the file name is not an identifier | add `as name` |
| `connection cycle detected` | `a` connects `b` connects `a` | move the shared part into a third file |
| `cannot read source file ... (named by #connect "x.csy")` | wrong path | paths are relative to the file with the `#connect` |
| `... is already the name of ...` | a namespace reuses an `#inc` alias | choose another name with `as` |

A call with the wrong number of arguments through a namespace is an error:
`'math::double' takes 1 argument(s), but this call passes 3`.

### Your own names for directives

`using` gives a directive another name in the current file:

```csy
using #inc as import;
using #connect as link;
using #explain as why;

import basic as bc;
link "lib/math.csy";
```

The new name is written without `#`. It has to be declared at the top level of the
file, before its first use; it belongs to that file only (connected files do not
inherit it); it cannot be a reserved word or the name of a function, variable or
namespace in the file; and it can be declared once. A mistyped alias is an ordinary
syntax error, not a silent comment.

### Explaining your code (#explain)

```csy
# in lib/math.csy
#explain double > "returns x times two" ref(answer);
#explain answer > "the number everything starts from";

# in lib/report.csy
#explain show > "prints a number doubled" ref(math::double);
```

- `#explain NAME > "text"` documents a function or a top-level variable.
  `ref(a, b, ns::c)` is optional and lists related names; names from connected
  files are written `namespace::name`.
- It can go anywhere in the file. No code is made from it: a program compiles to
  the same `.cso` with or without it.
- Read it with `csyc explain NAME --path FILE.csy`, which prints the text, the line
  where the name is declared and what it refers to:

  ```text
  "returns x times two" (func; line 3)

  REFER: answer(var; line 1)
  ```

- `csyc explain --path FILE.csy` (no name) lists every function and variable of the
  file; `--path project.csp` lists or searches every file of the project;
  `csyc explain math::double --path src/lib/report.csy` reaches into a connected file.
- A build warns about `#explain` lines whose names or refs no longer exist.
- `# explain ...` (with a space) and `#explaining` are ordinary comments; a
  malformed `#explain` is an error.

## Special Syntax

### Print Output

```csy
bc::out>value;  # Special > syntax for output
o>value;        # With alias: using bc::out as o
```

Output can be chained. Each `>` prints one more value on the same line, with no
separator, and the newline comes once at the end:

```csy
x = 5;
o>"x=">x>", half=">(x / 2.0);   # prints: x=5, half=2.5
```

**Comparisons and logic can follow `>` directly.** `==`, `!=`, `<`, `<=`, `>=`, `&&` and `||`
belong to the value being printed:

```csy
o>x == 5;                   # prints true
o>(x > 3) && (y < 9);       # wrap a comparison that uses > so it is not read as a separator
o>"ok=">x == 5;             # prints ok=true
```

A bare `>` always starts the next item of the chain, so a greater-than comparison needs parentheses
(`o>(x > 4);`). Arithmetic (`+ - * / %`) is safe without parentheses.

### Function Return

```csy
rt value;  # Return statement (short for return)
```

### Block End

```csy
::ext  # End of block (function, if, loop, class)
```

## Operator Precedence

From highest to lowest:
1. Function calls `func()`, indexing `[]`, field access `::`
2. Unary `!`, `-`
3. Multiplication `*`, Division `/`
4. Addition `+`, Subtraction `-`
5. Comparison `>`, `<`, `>=`, `<=`
6. Equality `==`, `!=`
7. Logical AND `&&`
8. Logical OR `||`
9. Assignment `=`, `+=`, `-=`

## Complete Example

```csy
#inc basic as bc;
using bc::out as o;

# Function definition
func factorial(n):
    if n <= 1:
        rt 1;
    ::ext
    rt n * factorial(n - 1);
::ext

# Class definition
class Counter(count):
    func increment(me):
        me::count += 1;
        rt me::count;
    ::ext
::ext

# Main code
nums = [1, 2, 3, 4, 5];
o>"factorials:";
for i in nums:
    o>factorial(i);
::ext

# Using class
c = Counter(0);
count = c::increment();
o>count;
count = c::increment();
o>count;

# Dict operations
tally = {"a": 0, "b": 0};
tally["a"] += 1;

# Conditionals
if count > 1:
    o>"count is big";
else:
    o>"count is small";
::ext

rt 0;
```

## Compiler Usage

```bash
# Compile and run
csyc main.csy

# Build project
csyc build project.csp

# Build with output
csyc build main.csy -o app.cso

# Run compiled object
csyc run app.cso

# Verbose build
csyc build --verbose

# Write the build log to a file
csyc build --log build.log

# Set memory limit
csyc build --memory-limit 1024

# Set thread count
csyc build --threads 4

# Pass arguments to program
csyc main.csy -- arg1 arg2

# Create a new project folder (project.csp, src/main.csy, .gitignore, ccm_table.bin)
csyc init myapp

# Print the #explain text of a name (no name lists everything)
csyc explain double --path src/lib/math.csy
csyc explain                       # inside a project folder, searches project.csp

# Install csyc itself, and check the system first
csyc doctor
csyc install --dry-run
csyc install [--force] [--prefix DIR] [--yes] [--no-deps]
                                   # default /usr; warns when another csyc on PATH shadows it

# Cooler Compiler (see the Cooler Compiler section)
csyc enable ccm
csyc disable ccm
csyc wipe --ccm-mem [--yes]

# Version (the word 'version' alone is not a command)
csyc --version
```

## Cooler Compiler

The Cooler Compiler is an optional extra voice for the compiler. It is off until you
turn it on. The compiler's own messages stay factual; when it is on, a short extra line
may follow them.

```bash
csyc enable ccm          # turn it on for this project folder
csyc disable ccm         # turn it off again
csyc wipe --ccm-mem      # forget everything it remembers
```

- The setting lives in `csyconf.json` in the project folder (`"cooler_compiler": true`).
  For a single `.csy` file, the folder of that file is used.
- The folder is the project folder, or for a single `.csy` file the folder that holds that file.
  A file in a subfolder such as `src/` has no `csyconf.json` there, so the Cooler Compiler stays
  silent for it, and prints no note either.
- The table must sit in the same folder. `csyc enable ccm` does not create it; in a folder
  without `ccm_table.bin` the compiler prints a note and compiles without the extra lines.
- `csyc init` places `ccm_table.bin` in the new project. It is a data file, not part of
  the compiler, and `csyc install` also keeps a copy next to the install (in
  `share/csyc/`) for `init` to copy.
- It remembers builds and runs in `ccm_mem.bin` (at most 500 records, oldest dropped
  first), including file paths and content fingerprints.
- The extra line is printed only when output goes to a terminal. In scripts, pipes and
  CI nothing is added.
- `csyc wipe --ccm-mem` always asks "are you sure" first, even when the Cooler Compiler
  is off, and only `y` counts as yes. When input is not a terminal it refuses unless you
  pass `--yes`.
- `ccm_mem.bin`, `ccm_mem.lock` and `csyconf.json` are not covered by the `.gitignore`
  that `csyc init` writes (it only ignores `*.cso`); add them yourself if you do not
  want them committed.

## Known Quirks

Behaviors that are easy to trip over. They are listed so you can work around them.

- A greater-than comparison needs parentheses when printed (see Print Output); the other
  comparisons and `&&` / `||` do not.
- `bc::min` and `bc::max` accept any number of numbers, or a single vector or f64_array.
- `bc::read_file` and `bc::read_lines` on a missing file return `none`. `bc::sqrt(-1)` gives `nan`.
- `bc::upper` / `bc::lower` only change ASCII letters.
- `bc::sum`, `bc::mean`, `bc::dot` and the other numeric builtins stop with an error on a value that is not a
  number. Builtins that take a count or a size (`bc::zeros`, `bc::ones`, `bc::eye`, `bc::seed`, `bc::randint`)
  need a whole number, and `bc::zeros`, `bc::ones` and `bc::eye` reject negative sizes. `bc::repeat` rejects a
  negative count, `bc::substr` a negative start or length, `bc::argmax` an empty vector, and `bc::shape` a ragged
  vector of vectors. `bc::randint` needs the lower bound first. `bc::sum` of ints is exact and stops with an
  error on overflow. `bc::float("1e999")` is an error, while `bc::float("inf")` gives `inf`.
- A parameter or class field that is declared twice is a compile error. Code after a top-level `rt` gets the
  unreachable code warning.
- `csyc build -o PATH` needs the folder of PATH to exist.
- `bc::mean` of an empty vector or f64_array is an error, `bc::min` and `bc::max` return `nan` when any value is `nan`,
  `bc::read_file` on a directory is an error, and `bc::exit` needs an integer.
- `==` and `!=` compare vectors, dicts and f64_arrays by contents, to any depth. A vector that
  contains itself cannot be compared and raises an error.
- Assigning a vector, dict or f64_array does not copy it. `b = a` followed by `b[0] = 5` also changes
  `a`. Use `bc::copy(a)` for an independent copy.
- A NaN is never equal to anything, itself included. Test for it with `bc::isnan(x)`, or `x != x`.
- A function that has its own parameter, loop variable or variable named like a math builtin (a
  parameter called `exp`, say) keeps `bc::exp(x)` calls interpreted, because native code could not tell
  the two apart. Rename the variable to keep the function native.
- A runtime error inside a natively compiled function that writes to an f64_array or i64_array
  reports the line of the call instead of the line that failed. Set `CSY_NO_NATIVE_JIT=1` to get
  the exact line.
- `bc::push` needs a vector. An f64_array has a fixed size, so pushing onto one is an error.
- Status lines such as `[csy] running ...` go to stderr, so redirecting stdout captures only
  what your program prints.

## Important Notes

1. **Statements end with `;`**. A missing `;` is a warning (the compiler inserts one), but write it. Block headers (`if ...:`, `func ...:`) and `::ext` take no semicolon
2. **Indentation is not significant** - blocks are delimited by `::ext`
3. **Implicit typing** - types are inferred from values
4. **Everything is an expression** - but only some contexts use the value
5. **Classes are sugar** - they desugar to dicts
6. **Module state** - functions can write module variables (stays interpreted)
7. **Native compilation** - numeric functions over f64_array can be JIT-compiled
8. **GPU support** - optional CUDA plugin for AI operations
9. **Error messages** - include file, line, and function information
10. **Resource limits** - compiler has memory and thread limits for safety

## Type System Notes

- Dynamic typing with runtime type tags
- `bc::typeof()` returns type as string
- Mixed int/float variables can have different type tags in native vs interpreted mode
- Use `CSY_JIT_STRICT_TYPES=1` for stricter type consistency

## Performance Considerations

- Numeric functions over `f64_array` compile to native code
- AI kernels (dense, matmul) are highly optimized
- String/vector operations run in bytecode VM (slower)
- Functions that write module variables stay interpreted
- Use packed arrays for AI model parameters
- GPU operations available for large matrix operations

For the change that matters most for speed (put loops inside functions) see Native JIT Eligibility. For the binary checkpoint format see Packed Float64 Arrays.

## Reserved Keywords

The following keywords are reserved in CSy:

- `func` - Function definition
- `if` - Conditional
- `orf` - Else-if (or-if)
- `else` - Else branch
- `while` - While loop
- `for` - For loop
- `in` - Loop iteration
- `thru` - Range iteration
- `class` - Class definition
- `stop` - Break out of loop
- `skip` - Continue to next iteration
- `rt` - Return
- `using` - Import alias
- `as` - Alias keyword
- `none` - Null value
- `true` - Boolean true
- `false` - Boolean false
- `extends` - Class inheritance
- `try` - Start a protected block
- `catch` - Handle an error raised inside the `try` block

## Additional Math Functions

```csy
# Trigonometric
bc::tan(x);           # Tangent
bc::asin(x);          # Arc sine
bc::acos(x);          # Arc cosine
bc::atan(x);          # Arc tangent
bc::atan2(y, x);      # Arc tangent of y/x (preserves quadrant)

# Logarithms
bc::log2(x);          # Base-2 logarithm
bc::log10(x);         # Base-10 logarithm

# Additional math
bc::sign(x);          # Sign of number (-1, 0, or 1)

# AI activation functions (work on a number, a vector or an f64_array)
bc::sigmoid(x);       # Sigmoid: 1 / (1 + exp(-x))
bc::relu(x);          # ReLU: max(0, x)
bc::tanh(x);          # Hyperbolic tangent

# Activation derivatives (given activation output y)
bc::sigmoid_deriv(y); # Derivative of sigmoid: y * (1 - y)
bc::relu_deriv(y);    # Derivative of ReLU: 1 if y > 0 else 0
bc::tanh_deriv(y);   # Derivative of tanh: 1 - y^2
```

## Additional Tensor Operations

```csy
# Random matrix
W = bc::rand_mat(rows, cols, lo, hi);  # Matrix of random floats in [lo, hi)

# Column-wise sum
db = bc::sum_cols(matrix);  # Sum down each column, returns 1D vector

# Argmax
best_idx = bc::argmax(vector);  # Index of largest element (vector or f64_array)
picks = bc::argmax(flat, cols); # f64_array of rows * cols values: one index per row

# Softmax
probs = bc::softmax(vector);  # Convert scores to probability distribution
# Works on 2D tensors (batch) row-by-row
probs = bc::softmax(flat, cols);  # f64_array of rows * cols values, row by row
```

## Functional Operations

```csy
# Map - apply function to each element
# NOTE: You must pass an actual function, not the keyword 'func'
doubled = bc::map(double_func, vec);

# Example with a named function
func double(x):
    rt x * 2;
::ext

numbers = [1, 2, 3, 4, 5];
doubled = bc::map(double, numbers);  # Use the function name, not 'func'

# Filter - keep elements where function returns truthy
evens = bc::filter(is_even, vec);

# Reduce - fold left with accumulator
total = bc::reduce(add, vec, initial);
```

**Important:**
- The first argument to `map`, `filter`, and `reduce` must be a function VALUE
- Do NOT use the keyword `func` as an argument
- Use the function NAME (without parentheses) to pass the function
- The function must be defined before passing it

## Additional String Operations

```csy
# Note: These are covered above but repeated for completeness
bc::upper(s);        # Uppercase
bc::lower(s);        # Lowercase
bc::trim(s);         # Trim whitespace
bc::repeat(s, n);    # Repeat string n times
bc::split(s, sep);   # Split by separator
bc::find(s, sub);    # Find substring index
bc::substr(s, start, len);  # Substring
bc::replace(s, from, to);   # Replace
```

## Additional Utility Functions

```csy
# Type checking
bc::typeof(value);   # Get type as string

# Indexing as function
value = bc::at(vec, idx);  # Same as vec[idx]

# Join
s = bc::join(vec, sep);    # Join vector elements with separator

# Contains
has = bc::contains(vec, elem);      # Check if element in vector
has = bc::contains(string, sub);   # Check if substring in string
has = bc::has(dict, key);          # Check if key in dict (alias)

# Dict operations
keys = bc::keys(dict);       # Get dict keys as vector
values = bc::values(dict);   # Get dict values as vector

# Reverse
rev = bc::reverse(vec);      # Reverse vector
rev = bc::reverse(string);  # Reverse string

# Sort
sorted = bc::sort(vec);      # Sort numbers or strings

# Dict merge
merged = bc::dict_merge(dict1, dict2);  # Merge dicts, second overrides first

# Environment and standard input
home = bc::getenv("HOME");   # the value, or none if the variable is not set
text = bc::read_stdin();     # everything left on standard input as one string
```

## Additional File Operations

```csy
# Append to file
bc::append_file(path, content);  # Append content to file

bc::read_lines(path);     # the lines of a file as a vector of strings, or none if it cannot be read
bc::list_dir(path);       # names in a directory, sorted; an error if it cannot be read
bc::is_dir(path);         # true if the path is a directory
bc::make_dir(path);       # creates the directory and any missing parents; true on success
bc::remove_file(path);    # removes a file; false if it was not there; an error for a directory
```

## Time Functions

```csy
# Sleep
bc::sleep(seconds);  # Sleep for specified seconds (capped at 1 day)
```

## Pi Digits

```csy
digits = bc::pi_digits(50);   # pi as a string of digits
```

`bc::pi_digits` needs the GMP C++ library when csyc is built. Without it, the call stops
with "bc::pi_digits requires GMP"; rebuild csyc with GMP installed.

## TUI (Terminal UI) Module

The TUI module provides terminal control without external libraries.

```csy
#inc tui;
```

### Screen Control

```csy
tui::clear();              # Clear screen
tui::goto(row, col);       # Move cursor to position
tui::hide_cursor();        # Hide cursor
tui::show_cursor();        # Show cursor
tui::alt_screen_on();      # Enter alternate screen buffer
tui::alt_screen_off();     # Leave alternate screen buffer
tui::flush();              # Flush output
```

### Text Styling

```csy
tui::bold();               # Bold text
tui::dim();                # Dim text
tui::reset();              # Reset all styles
tui::color(code);          # Set SGR color code (generic escape hatch)
```

### True Color

```csy
tui::rgb(r, g, b);         # Set foreground color (0-255 each)
tui::bg_rgb(r, g, b);      # Set background color (0-255 each)
```

### Terminal Size

```csy
rows = tui::rows();         # Get terminal rows
cols = tui::cols();         # Get terminal columns
size = tui::size();         # Get [rows, cols] as vector
```

### Raw Input Mode

```csy
tui::raw_on();             # Enable character-at-a-time input, no echo
tui::raw_off();            # Disable raw mode
```

### Non-blocking Input

```csy
key = tui::key();          # Get key press (non-blocking, returns "" if none)
```

**TUI Safety Notes:**
- Raw mode and alternate screen are automatically restored on exit (normal, error, or signal)
- Safe to Ctrl+C out of programs using TUI
- Works on any Unix-like system with POSIX termios

## Module Import: tui

```csy
#inc tui;
```

The TUI module is always available (no external dependencies needed).

## Compiler Directives

```csy
#inc module as alias;    # Import module with alias
#connect "file.csy";     # Connect another .csy file (namespace = file name)
#connect "dir/file.csy" as name;            # ... under another namespace name
#connect "file.csy" as name { exports; }    # ... sharing only the listed names
#explain name > "text" ref(a, ns::b);       # Document a function or variable (csyc explain)
using #inc as import;    # Your own name for a directive, for this file
```

## All Operators

### Arithmetic
- `+` - Addition
- `-` - Subtraction
- `*` - Multiplication
- `/` - Division
- `%` - Modulo

### Assignment
- `=` - Assignment
- `+=` - Add and assign
- `-=` - Subtract and assign

`*=`, `/=` and `%=` are not supported.

### Comparison
- `==` - Equal
- `!=` - Not equal
- `>` - Greater than
- `<` - Less than
- `>=` - Greater or equal
- `<=` - Less or equal

### Logical
- `&&` - Logical AND
- `||` - Logical OR
- `!` - Logical NOT

### Punctuation
- `;` - Semicolon (statement terminator for rt, etc.)
- `:` - Colon (block start, type hints)
- `.` - Dot (member access for dicts)
- `::` - Double colon (namespace/field access)
- `,` - Comma (separator)
- `(` `)` - Parentheses (function calls, grouping)
- `[` `]` - Brackets (indexing, vectors)
- `{` `}` - Braces (dicts, connect allowlist)

### Special
- `>` - Output to stdout (with bc::out or aliased name)
- `::ext` - Block end marker

## Token Types

The lexer recognizes:
- Integers: `42`, `-10`
- Floats: `3.14`, `-0.5`, `2.0`
- Strings: `"hello"`, `"world"`
- Identifiers: `myVar`, `func_name`
- Keywords: see Reserved Keywords section
- Operators: see All Operators section
- Comments: `# comment to end of line`
- Directives: `#inc`, `#connect`, `#explain`

## Compiler Options

```bash
# Build options
csyc build --verbose                    # Detailed build log
csyc build --log build.log             # Also write every stage, diagnostic and timing to a file
csyc build --memory-limit 1024         # Set memory budget in MiB
csyc build --threads 4                 # Set worker thread count
csyc build --no-native                 # Disable native compilation
csyc build --exe                       # Create standalone executable
csyc build main.csy --native-exe -o app # Experimental strict native-only executable
csyc build -o output.cso               # Specify output file

# Environment variables
CSYC_MEMORY_LIMIT_MB=2048             # Default memory budget
CSYC_THREADS=auto                      # Default thread count
CSY_PROFILE=1                          # Enable profiling
CSY_MEM_TRACE=1                       # Trace memory usage
CSY_JIT_DEBUG=1                        # Debug JIT compilation
CSY_NO_NATIVE_JIT=1                    # Disable native JIT
CSY_JIT_STRICT_TYPES=1                # Strict type consistency
CSY_NO_AUTO_CUDA=1                    # Disable auto CUDA setup
CSY_ALLOW_CWD_LIBS=1                   # Allow libraries from current directory
```

## Reading Compiler Messages

Every error and warning has the same layout:

```
  main.csy:4:5  error
  ----------------------------------------
  'x' is being read before it has a value, that can't work

  3 | y = 5;
  4 | x = x + 1;
    |     ^
  5 | say>x;

  fix: give 'x' a value on an earlier line, like x = 0;
```

- The first line gives the file, line, column and kind. The dashed line and the one-line message
  follow it.
- The excerpt shows the line before and after, and underlines the exact word with `^~~~`.
- `fix:` suggests what to change. `hint:` adds extra clues such as a close spelling.
- A runtime error that happens inside functions ends with `how it got here, newest first:` and
  lists each caller with its line number.
- Errors from built-in functions, file loading, data formats and external commands use the same
  layout and also get a `fix:` line.
- The last line counts the errors and warnings. At most 100 errors and 200 warnings are printed.

`csyc build --log build.log` writes a plain text record of the build to a file: each stage with the
time since the build began, the memory in use and the peak, every error and warning with its fix and
call chain, and whether the program ran and how it ended. The file is written line by line, so a
build that stops early or crashes still leaves everything up to that point. When a build fails, the
last thing printed is the path of the log. The log file may not end in `.csy` and cannot replace
anything that is not a regular file.

## Error Handling

```csy
# Raise custom error
bc::raise("error message");

# Runtime errors include location
# Example: "vector index 9 out of bounds (size 3) (in function 'f', line 4)"
```

The error header shows the file, line and column where the failure happened, with the source line
and a caret, including for errors inside functions of connected files.

### try and catch

`try` and `catch` are implemented. If anything inside the `try` block raises an error
(`bc::raise`, a runtime error such as division by zero or an out-of-range index), the
`catch` block runs with the error message bound to the name you give it, as a string.

```csy
#inc basic as bc;

try:
    bc::raise("boom");
catch e:
    bc::out>"caught: " + e;
::ext
```

- One `::ext` closes the whole `try ... catch ...` statement.
- `catch` needs a name (`catch e:`); `try` without `catch` is a syntax error.
- It works inside functions, loops (`skip` and `stop` still work) and nested `try` blocks.
  Raising again from a `catch` block is caught by an outer `try`.
- Errors found while compiling (syntax errors, unknown names) are not catchable; only
  errors raised while the program runs are.

## Type Coercion

CSy performs automatic type coercion in some contexts:

- Numbers to string: `bc::str(42)` → `"42"`
- String to number: `bc::int("123")` → `123`, `bc::float("3.14")` → `3.14`
- Bool to number: `true` → `1`, `false` → `0`
- Number to bool: non-zero → `true`, zero → `false`
- Float to int: truncates (raises error if out of range)
- Int to float: converts exactly

## Value Display Format

When printed or converted to string:
- Integers: printed as decimal
- Floats: printed with decimal point
- Strings: printed with quotes in some contexts
- Booleans: `true` or `false`
- None: `none`
- Vectors: `[1, 2, 3]`
- Dicts: `{"key": "value"}`
- Functions: `<function name>`
- F64Arrays: printed as vector of floats

## Limits and Caps

The compiler enforces safety limits:

- Source file size: 32 MiB max
- Total source: 256 MiB max
- Number of files: 4096 max
- Connection levels: 256 max
- Tokens per file: 48 million max
- Parenthesis and expression nesting: 255 levels max (the error says 256)
- Expression chain length: 1000 max, so `1+1+...` works up to 999 terms and fails at 1001
- Statement nesting: 255 levels max (`if` inside `if` inside ...)
- Builtin arguments: 1000 max
- Tensor elements: 134 million max
- File read size: 1 GiB max
- Object file size: 512 MiB max
- Diagnostics: 100 errors, 200 warnings per file max

## Native JIT Eligibility

Functions are eligible for native JIT compilation if they:
- Use numeric operations over integers, floats, `f64_array` or `i64_array`
- Call whitelisted builtins, as `sqrt(x)` or `bc::sqrt(x)` (see below), and supported numeric
  conversions such as `bc::int`
- Don't read/write module variables
- Have a compatible signature

Use `--explain-native` to see why functions stay interpreted.

An otherwise-native function can also call the interpreter for an isolated `bc::len(parameter)`
expression when that parameter is not indexed as a numeric array. This lets a numeric function use
the length of a string, vector or dict without moving its other supported work out of native code.
The original parameter and its object identity are preserved. This is a narrow fallback, not
general native support for string, vector or dict operations; other unsupported expressions keep
the function interpreted.

Functions that index an ordinary vector parameter may be specialized for native execution when the
argument is provably a non-empty homogeneous vector of integers or floats. The values are copied
into a temporary packed buffer and copied back after native mutations. Mixed or empty vectors and
repeated aliases are left interpreted.

### The rules that decide speed

- **Put hot loops inside functions.** A top-level loop is only compiled natively when it is simple
  enough, and a loop that indexes `table["columns"][name]` is not. Copying 30 Parquet columns of
  500,000 rows into one flat array took 2.97 s in a top-level loop and 0.24 s once the loop body
  was a function that takes the arrays and numbers as arguments.
- **`sqrt(x)` and `bc::sqrt(x)` are the same to native code.** Both work, in functions and in
  top-level loops, whatever name you gave the `#inc basic` alias. A loop of 4 million
  `bc::sin` and `bc::sqrt` calls ran in 0.065 s, against 0.96 s when `bc::` calls kept the loop
  interpreted. The one exception is a function that declares its own variable, parameter or loop
  variable with the same name as the builtin; `--explain-native` reports that case.
- **The whitelist** is: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sqrt`, `exp`, `log`, `log2`, `log10`, `floor`, `ceil`, `round`, `abs`, `sign`, `tanh`, `sigmoid`, `relu`, `sigmoid_deriv`, `relu_deriv`, `tanh_deriv`, `atan2`, `pow`, `clamp`, `max`, `min`, `isnan`, `isinf`. `max` and `min` stay native only with exactly two
  numeric arguments; with more arguments or an array they run in the interpreter, which gives the
  same answer more slowly.
- **A function may return `true`, `false` or a comparison**, and `while true:` loops work, so
  predicates such as `is_prime(n)` are native. A call to such a function can only be an `if` or
  `while` condition. A comparison, `isnan` or `isinf` result, or a call to a boolean function,
  cannot be stored in a variable or used as a number; that keeps the function interpreted. Count with
  `if isnan(x): c += 1;` rather than `c += isnan(x);`. A function must return booleans on every
  path or numbers on every path, not a mix.
- **Float `%` stays interpreted.** Integer `%` is native.
- **General string, vector and dict operations remain interpreted.** The `len` fallback above and
  proven homogeneous-vector specialization are limited exceptions. Keep other collection handling
  outside hot functions.

## GPU Module (NVIDIA)

```csy
#inc nvidia as nv;  # Deprecated, use #inc ai as ai
```

Hardware queries still work through `nv::`. Each takes an optional device index
(default 0) and returns `-1` (numbers), `"n/a"` or `"none"` (text) when no GPU is present:

```csy
nv::available();           # true or false
nv::device_count();
nv::name();                # "none" when there is no GPU
nv::info();                # dict with all of the fields below
nv::driver_version();
nv::compute_capability();
nv::memory_total();        # bytes
nv::memory_free();
nv::memory_used();
nv::temperature();         # Celsius
nv::utilization();         # percent
nv::power();               # watts
```

Note: The `nvidia` module is deprecated. Use the `ai` module for GPU operations instead.

The GPU module requires CUDA and is optional. It auto-downloads CUDA libraries on first use unless `CSY_NO_AUTO_CUDA=1` is set.

## Deprecated Features

### 1. NVIDIA/CUDA Module Imports

**Deprecated:** `#inc nvidia as nv;` and `#inc cuda as nv;`

**Use instead:** `#inc ai as ai;`

**Why:** The NVIDIA module is being consolidated into the AI module for a more unified API.

**Old code:**
```csy
#inc nvidia as nv;
result = nv::matmul(a, b);
```

**New code:**
```csy
#inc ai as ai;
result = ai::matmul(a, b, rows, inner, cols);
```

**Note:** The old `nv::` functions remain supported for compatibility, but new code should use the `ai::` module.

### 2. Basic Module Matrix Multiplication

**Deprecated:** `bc::matmul()` for performance-sensitive AI workloads

**Use instead:** `ai::matmul()` with packed `f64_array` buffers

**Why:** The packed `f64_array` operations are significantly faster for AI workloads.

**Old code:**
```csy
#inc basic as bc;
A = [[1, 2], [3, 4]];
B = [[5, 6], [7, 8]];
result = bc::matmul(A, B);  # Works with nested vectors, slower
```

**New code:**
```csy
#inc basic as bc;
#inc ai as ai;

# Use packed f64_array for better performance
A = bc::f64_array(6);  # 2x3 in row-major
A[0] = 1.0; A[1] = 2.0; A[2] = 3.0;
A[3] = 4.0; A[4] = 5.0; A[5] = 6.0;

B = bc::f64_array(6);  # 3x2 in row-major
B[0] = 7.0; B[1] = 8.0;
B[2] = 9.0; B[3] = 10.0;
B[4] = 11.0; B[5] = 12.0;

result = ai::matmul(A, B, 2, 3, 2);  # Much faster
```

**Note:** `bc::matmul` remains supported for non-performance-critical code or when working with nested vectors.

### 3. Compiler Flag: --deep

**Deprecated:** `--deep` flag

**Use instead:** `--verbose` flag

**Why:** The flag was renamed for clarity.

**Old command:**
```bash
csyc build --deep
```

**New command:**
```bash
csyc build --verbose
```

### 4. GPU Training Functions

**Deprecated:** Direct `nv::` calls for MLP training

**Use instead:** `ai::mlp_train_gpu()` for GPU training

**Why:** The AI module provides a more unified and higher-level API for training.

**Old approach:**
```csy
#inc nvidia as nv;
# Manual loops with nv::matmul for training
```

**New approach:**
```csy
#inc ai as ai;
loss = ai::mlp_train_gpu(X, Y, W1, b1, W2, b2,
                         batch, input_width, hidden_width, output_width,
                         epochs, learning_rate, "sigmoid");
```

## Migration Guide

### When to Use Packed Arrays vs Nested Vectors

**Use packed `f64_array` when:**
- Performance-critical AI/ML workloads
- Training neural networks
- Large matrix operations
- GPU computation

**Use nested vectors when:**
- Small, non-performance-critical operations
- Convenience and readability matter more than speed
- Working with heterogeneous data
- Quick prototyping

### Example: Complete Migration

**Before (deprecated):**
```csy
#inc basic as bc;
#inc nvidia as nv;

# Nested vector matmul
A = [[1, 2], [3, 4]];
B = [[5, 6], [7, 8]];
result = nv::matmul(A, B);
```

**After (recommended):**
```csy
#inc basic as bc;
#inc ai as ai;

# Packed array matmul
A = bc::f64_array(4);  # 2x2
A[0] = 1.0; A[1] = 2.0;
A[2] = 3.0; A[3] = 4.0;

B = bc::f64_array(4);  # 2x2
B[0] = 5.0; B[1] = 6.0;
B[2] = 7.0; B[3] = 8.0;

result = ai::matmul(A, B, 2, 2, 2);
```

### Backward Compatibility

All deprecated features remain supported:
- Old code will continue to work
- Warnings are informational only
- No breaking changes in current version
- Migration can be done incrementally

## Common Errors and Pitfalls

### Error: "expected a method ('func ...') inside class"

**Problem:** Using `var` declarations inside a class body.

```csy
# INCORRECT
class Model(name):
    var name = name;        # ERROR
    var weights = bc::f64_array(4);  # ERROR
    func initialize(me):
        rt 0;
    ::ext
::ext
```

**Solution:** Classes can only contain method definitions. Initialize fields inside methods instead.

```csy
# CORRECT
class Model(name):
    func initialize(me):
        me::weights = bc::f64_array(4);  # Initialize in method
        rt 0;
    ::ext
::ext
```

**Key points:**
- Class parameters automatically become instance fields
- Additional fields must be assigned in methods using `me::field = value`
- No `var` declarations allowed in class bodies

### Error: "expected an expression but got 'func'"

**Problem:** Using the keyword `func` as an argument to functional operations.

```csy
# INCORRECT
doubled = bc::map(func, numbers);  # ERROR
```

**Solution:** Pass the function name (without parentheses), not the keyword.

```csy
# CORRECT
func double(x):
    rt x * 2;
::ext

doubled = bc::map(double, numbers);  # Use function name
```

**Key points:**
- `map`, `filter`, and `reduce` expect a function VALUE
- Pass the function NAME, not the keyword `func`
- The function must be defined before passing it
- Do not include parentheses when passing the function

### Error: "unknown builtin" / "has no member"

**Problem:** Trying to use a builtin that doesn't exist or is misspelled. The compiler usually
catches this before the program runs, with `module 'basic' has no member 'name'` and a list of close
names. `unknown builtin` is the same mistake caught at run time.

```csy
# INCORRECT
result = bc::nonexistent();  # ERROR
```

**Solution:** Check the builtin name spelling and verify it exists in the documentation.

### Error: "vector index out of bounds"

**Problem:** Accessing an index that doesn't exist in a vector.

```csy
# INCORRECT
nums = [1, 2, 3];
x = nums[5];  # ERROR: index 5 out of bounds (size 3)
```

**Solution:** Ensure index is within bounds: `0 <= index < len(vec)`. Negative indexes count from the end.

### Error: "cannot compare numbers with strings"

**Problem:** Trying to sort a mixed-type vector.

```csy
# INCORRECT
mixed = [1, "hello", 2.5];
sorted = bc::sort(mixed);  # ERROR
```

**Solution:** Sort homogeneous vectors only - all numbers or all strings.

### Error: "range expects one number"

**Problem:** `bc::range(2, 5)` or `bc::range(0, 10, 3)`. CSy has only `range(n)`, which gives 0 to
n - 1.

**Solution:** Build the sequence with `for i in thru n:` and compute `start + i * step`.

### Error: "tensor nesting is too deep"

**Problem:** Creating a tensor with more than 256 levels of nesting.

**Solution:** Check for recursive vector structures or reduce nesting depth.
