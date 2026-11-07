#include "voice.hpp"

#include <regex>
#include <string>
#include <vector>

namespace csy {

namespace {

struct Rule {
    std::regex pattern;
    std::string text;
    std::string fix;
};

Rule rule(const char* pattern, const char* text, const char* fix = "") {
    return Rule{std::regex(pattern, std::regex::ECMAScript), text, fix};
}

const std::vector<Rule>& rules() {
    static const std::vector<Rule> table = {
        rule(R"RX(^expected '&&' but got '&'$)RX", "that's a single '&', CSy wants '&&' for logical and, no cap", "write '&&' between the two conditions"),
        rule(R"RX(^expected '\|\|' but got '\|'$)RX", "that's a single '|', CSy wants '||' for logical or, no cap", "write '||' between the two conditions"),
        rule(R"RX(^file has more than (.*) tokens - split it into smaller source files$)RX", "this file has way more than $1 tokens, that's too much for one file", "split it into smaller .csy files and pull them together with #connect"),
        rule(R"RX(^malformed number literal - too many decimal points$)RX", "that number has too many decimal points, bro, one dot max", "keep a single '.' in the number, like 3.14"),
        rule(R"RX(^invalid \\(.) escape: expected (.*) hex digits(.*)$)RX", "that \\$1 escape is busted, it needs exactly $2 hex digits$3", "write it like \\x41, \\u00e9 or \\U0001F600"),
        rule(R"RX(^unterminated string literal.*$)RX", "this string never closes, you forgot the ending double quote", "add a closing \" at the end of the string"),
        rule(R"RX(^unexpected byte 0x([0-9a-fA-F]+) \((.*)\)$)RX", "found a stray byte 0x$1 out here, $2", "delete it, or move it inside a string if it is meant as text"),
        rule(R"RX(^unexpected character '(.*)'$)RX", "'$1' isn't a thing in CSy, no idea what to do with it", "remove it, or put it inside a string if you meant it as text"),

        rule(R"RX(^expected '::ext' but got '<eof>'$)RX", "this block never got closed, the file ended while I was still waiting for '::ext'", "every if, while, for, func, class and try ends with ::ext, add one where the block should stop"),
        rule(R"RX(^expected an expression but got '<eof>'$)RX", "I needed a value here but the file just ended", "finish the statement, or delete the dangling operator"),
        rule(R"RX(^expected (.*) but got '<eof>'$)RX", "I needed $1 here but the file just ended", "finish the statement before the end of the file"),
        rule(R"RX(^expected '::ext' but got '(.*)'$)RX", "this block never got closed, I hit '$1' while still waiting for '::ext'", "every if, while, for, func, class and try ends with ::ext, add one where the block should stop"),
        rule(R"RX(^unexpected '::ext' - no matching block to close$)RX", "this '::ext' is closing nothing, there's no open block here", "delete it, or look for a block you closed too early"),
        rule(R"RX(^'elif' isn't CSy - this language uses 'orf' instead$)RX", "'elif' isn't a thing here, CSy says 'orf'", "swap 'elif' for 'orf'"),
        rule(R"RX(^expected module name after '#inc'$)RX", "#inc needs a module name right after it", "write it like: #inc basic as bc;"),
        rule(R"RX(^expected alias name after 'as'$)RX", "'as' needs a name after it, I got nothing", "write the name you want, like: as bc"),
        rule(R"RX(^expected a quoted \.csy path after '#connect'$)RX", "#connect wants a quoted .csy path and I didn't get one", "write it like: #connect \"util.csy\";"),
        rule(R"RX(^expected a namespace name after 'as'$)RX", "'as' needs a namespace name after it", "pick a plain identifier, like: #connect \"util.csy\" as util;"),
        rule(R"RX(^the file name in '(.*)' cannot be used as a namespace name;(.*)$)RX", "the file name in '$1' can't be a namespace name, it isn't a valid identifier;$2", "add 'as name' after the path so the namespace gets a proper name"),
        rule(R"RX(^expected a function or variable name in the '#connect' allowlist$)RX", "the #connect allowlist needs a function or variable name here", "list the names you want, separated by commas"),
        rule(R"RX(^expected '\}' to close the '#connect' allowlist$)RX", "the #connect allowlist never closes, I'm missing the '}'", "add a '}' after the last name"),
        rule(R"RX(^a directive alias can only be declared at the top level of a file$)RX", "directive aliases only work at the top of a file, not inside a block", "move this line to the top level"),
        rule(R"RX(^expected 'as' and the new name after '(.*)'$)RX", "after '$1' I need 'as' and the new name", "write it like: using $1 as shortName;"),
        rule(R"RX(^expected the new name for '(.*)' after 'as'$)RX", "'as' needs the new name for '$1' after it", "type the name you want right after 'as'"),
        rule(R"RX(^expected 'as' after using-expression$)RX", "this using line needs 'as' and a name", "write it like: using bc::out as say;"),
        rule(R"RX(^expected function name after 'func'$)RX", "'func' needs a name right after it", "write it like: func add(a, b):"),
        rule(R"RX(^expected parameter name$)RX", "I expected a parameter name here and got something else", "parameters are plain names separated by commas, like (a, b)"),
        rule(R"RX(^expected class name after 'class'$)RX", "'class' needs a name right after it", "write it like: class Animal(name):"),
        rule(R"RX(^expected a field name$)RX", "I expected a field name here", "fields are plain names inside the parentheses, like (name, age)"),
        rule(R"RX(^expected base class name after 'extends'$)RX", "'extends' needs a base class name after it", "write it like: class Dog(name) extends Animal(name):"),
        rule(R"RX(^class '(.*)' cannot extend itself$)RX", "class '$1' can't extend itself, that's a loop with no exit", "pick a different base class or remove 'extends'"),
        rule(R"RX(^class '(.*)' extends itself through its base classes$)RX", "class '$1' ends up extending itself through its base classes, that's a circle", "break the chain so no class is its own ancestor"),
        rule(R"RX(^expected a method \('func \.\.\.'\) inside class '(.*)'$)RX", "class '$1' wants methods inside it, and this isn't one", "start each method with 'func name(me, ...):'"),
        rule(R"RX(^expected 'catch' after 'try' block$)RX", "a 'try' block needs a 'catch' right after it", "add 'catch e:' after the try block"),
        rule(R"RX(^expected a name to bind the error to after 'catch'$)RX", "'catch' needs a name for the error, like 'catch e:'", "write 'catch e:' and use 'e' inside the block"),
        rule(R"RX(^expected loop variable name after 'for'$)RX", "'for' needs a loop variable name after it", "write it like: for item in items:"),
        rule(R"RX(^expected 'in' after loop variable$)RX", "a 'for' loop needs 'in' after the loop variable", "write it like: for item in items:"),
        rule(R"RX(^expected member name after '::'$)RX", "'::' needs a name after it", "write something like bc::len or obj::field"),
        rule(R"RX(^expected an expression but got '(.*)'$)RX", "I needed a value or expression here but found '$1'", "put a value, a name or a call where '$1' is, or check the line above for something unfinished"),
        rule(R"RX(^expected (.*) but got '(.*)'$)RX", "ok so I needed $1 here but found '$2' instead, not it", "check the spot right before this for a typo or a missing piece"),
        rule(R"RX(^maximum (.*) nesting depth exceeded \(256\)$)RX", "this is nested 256 levels deep and that's the limit, way too much", "pull the inner part out into its own function"),
        rule(R"RX(^too many errors in this file - stopping here$)RX", "that's a lot of errors, I'm tapping out on this file", "fix the first few and run it again, the rest are often knock-on effects"),
        rule(R"RX(^missing ';' - the compiler is treating one as inserted here$)RX", "missing a ';' here, I'm pretending it's there so we can keep going", "add the ';' at the end of the previous statement"),

        rule(R"RX(^namespace '(.*)' is already declared in this file$)RX", "namespace '$1' already exists in this file, can't have two", "give one of them a different name with 'as'"),
        rule(R"RX(^connection namespace '(.*)' is declared more than once$)RX", "namespace '$1' got declared more than once", "give each #connect its own name with 'as'"),
        rule(R"RX(^'(.*)' is already defined as a function - (.*)$)RX", "'$1' is already a function, so assigning to it here won't do what you think, $2", "use a different variable name"),
        rule(R"RX(^'(.*)' is already defined as a (.*) - this redefines it, the earlier one is gone$)RX", "'$1' is already defined as a $2, redefining it deletes the earlier one", "rename one of them if you need both"),
        rule(R"RX(^unreachable code: this follows '(.*)' in the same block and can never run$)RX", "this code can never run, it sits right after '$1'", "delete it, or move it above the '$1'"),
        rule(R"RX(^cannot assign to '(.*)': names that belong to a module or a #connect are read-only$)RX", "you can't assign to '$1', names from modules and #connect are read only", "make your own variable instead, like myValue = 5;"),
        rule(R"RX(^cannot assign to a literal value; .*$)RX", "you can't assign to a literal, the left side of '=' has to be a variable, an element or a field", "put a variable name on the left, like x = 5;"),
        rule(R"RX(^the left side of '=' must be a variable, an element or a field$)RX", "the left side of '=' has to be a variable, an element or a field, this isn't one", "write a name like x, an element like v[0] or a field like me::x"),
        rule(R"RX(^unknown module '(.*)', only (.*) are supported\. The alias '(.*)' will be created but it has no members\.$)RX", "module '$1' doesn't exist, the only ones are $2, so the alias '$3' is empty", "check the module name spelling after #inc"),
        rule(R"RX(^unsupported 'using' target - only `using <module>\.<member> as <name>` is supported in v1$)RX", "that 'using' target isn't supported, only 'using module::member as name' works right now", "write it like: using bc::out as say;"),
        rule(R"RX(^(.*) assigns a variable to itself and does nothing$)RX", "$1 assigns a variable to itself, that does literally nothing", "delete the line or assign something different"),
        rule(R"RX(^assigning to loop variable '(.*)' does not change how many times the loop runs \(it is reset on every round\)$)RX", "changing loop variable '$1' doesn't change how many rounds the loop runs, it resets every round", "use a while loop if you need to control the count yourself"),
        rule(R"RX(^this 'if' condition is a constant that is always (true|false) - one branch can never run$)RX", "this 'if' condition is always $1, so one branch can never run", "use a real condition, or remove the branch that never runs"),
        rule(R"RX(^this 'while' condition is always false - the loop body never runs$)RX", "this 'while' condition is always false so the body never runs, ever", "fix the condition or delete the loop"),
        rule(R"RX(^'(stop|skip)' used outside of a loop - has no effect here$)RX", "'$1' only does something inside a loop and this isn't in one", "move it inside a for or while loop, or delete it"),
        rule(R"RX(^'\.' is the old CSy member access - use '::' instead \(e\.g\. bc::out, not bc\.out\)$)RX", "'.' isn't how you reach members in CSy anymore, it's '::'", "write bc::out, not bc.out"),
        rule(R"RX(^'(.*)' is already an alias for (.*) \(declared on line (.*)\)$)RX", "'$1' is already an alias for $2, you set that up on line $3", "pick a different alias name"),
        rule(R"RX(^the alias '(.*)' for (.*) is already used as (.*) in this file$)RX", "the alias '$1' for $2 is already taken, it's used as $3 in this file", "pick a different alias name"),
        rule(R"RX(^expected ',' or '\)' in ref\(\.\.\.\)$)RX", "ref(...) needs ',' between names and a ')' at the end", "write it like ref(name1, name2)"),
        rule(R"RX(^expected a function or variable name inside ref\(\.\.\.\)$)RX", "ref(...) wants a function or variable name here", "put a plain name inside the parentheses"),
        rule(R"RX(^expected '>' and the explanation in quotes after '#explain (.*)'$)RX", "#explain $1 needs '>' and then the explanation in quotes", "write it like: #explain $1 > \"what it does\""),
        rule(R"RX(^expected the explanation in quotes after '>'$)RX", "after '>' I need the explanation in double quotes", "wrap the explanation in \"quotes\""),
        rule(R"RX(^expected the function or variable name to explain after '#explain'.*$)RX", "#explain needs the name of the thing it explains", "write it like: #explain myFunc > \"what it does\""),
        rule(R"RX(^ref\(\.\.\.\) lists more than (.*) names$)RX", "ref(...) lists more than $1 names, that's the limit", "split it into several explanations"),
        rule(R"RX(^the explanation is longer than (.*) characters$)RX", "that explanation is longer than $1 characters, which is the limit", "shorten it"),
        rule(R"RX(^expression is too long or too deeply chained \(limit (.*) levels\) - split it into several statements$)RX", "that expression is chained more than $1 levels deep, way too much", "split it into several statements"),
        rule(R"RX(^integer literal '(.*)' does not fit in a 64-bit int \(range .*\)$)RX", "'$1' is too big to be a 64-bit int", "write it as a float, like 1e19, if an approximate value is fine"),
        rule(R"RX(^number literal '(.*)' is outside the range of a 64-bit float$)RX", "'$1' is outside what a 64-bit float can hold", "use a smaller number"),
        rule(R"RX(^missing '#' before 'inc' - treating this as '#inc'$)RX", "'inc' needs a '#' in front, I'm treating it as '#inc' for now", "write #inc, with the '#'"),
        rule(R"RX(^the (.*) block that starts on line (.*) is never closed with '::ext'$)RX", "the $1 block that starts on line $2 never gets closed", "add '::ext' where that block should end"),
        rule(R"RX(^values have no methods in CSy, so '(.*)' will not work.*$)RX", "values don't have methods in CSy, so '$1' won't work", "call a function from the basic module instead, like bc::push(items, x)"),
        rule(R"RX(^unknown name '(.*)'$)RX", "'$1' isn't a thing here, never heard of it", "check the spelling, or give '$1' a value before this line"),
        rule(R"RX(^'(.*)' is read before it has been given a value \((.*)\)$)RX", "'$1' gets read before it has a value, $2", "set '$1' before the loop starts"),
        rule(R"RX(^'(.*)' is read before it has been given a value$)RX", "'$1' is being read before it has a value, that can't work", "give '$1' a value on an earlier line, like $1 = 0;"),
        rule(R"RX(^'(.*)' is a module name, not a value; write (.*)::name to use something from it$)RX", "'$1' is a module, not a value, you can't just use it by itself", "write $1::name to grab something from it, like $1::len"),
        rule(R"RX(^'(.*)' takes (.*), but this call passes (.*)$)RX", "'$1' wants $2 but you gave it $3, the numbers aren't numbering", "pass exactly as many arguments as the function lists"),
        rule(R"RX(^(division|remainder) by a literal zero - this always fails when it runs$)RX", "$1 by a literal zero, that blows up every single time", "use a non-zero divisor"),
        rule(R"RX(^'(.*)' is not exposed by connection '(.*)'$)RX", "'$1' isn't shared by '$2', so you can't reach it from here", "add it to the #connect allowlist, or remove the leading '_' if it is private"),
        rule(R"RX(^module '(.*)' has no member '(.*)'$)RX", "module '$1' has nothing called '$2'", "check the spelling of '$2'"),
        rule(R"RX(^duplicate dictionary key "(.*)" - the later value replaces the earlier one$)RX", "the key \"$1\" shows up twice in this dict, the later value wins and the first one is gone", "delete one of them or rename a key"),
        rule(R"RX(^names that start with '_' are private to their file and are never shared by #connect$)RX", "names starting with '_' are private to their file, #connect won't share them"),

        rule(R"RX(^a built-in function call may have at most 999 arguments \(this one has (.*)\)$)RX", "that call has $1 arguments and the max is 999, way too many", "pass a vector instead of that many separate arguments"),
        rule(R"RX(^destructuring targets must be plain names, like \[a, b\] = \.\.\.$)RX", "destructuring only works with plain names on the left, like [a, b] = ...", "use simple variable names inside the brackets"),

        rule(R"RX(^division by zero$)RX", "you divided by zero, the universe said no", "check that the divisor isn't 0 before dividing"),
        rule(R"RX(^modulo by zero$)RX", "you took a remainder by zero, same energy as dividing by zero, a no from the universe", "check that the divisor isn't 0 first"),
        rule(R"RX(^integer overflow in (.*)$)RX", "that $1 went past the biggest number an int can hold", "use floats for huge numbers, or keep the values smaller"),
        rule(R"RX(^cannot (add|subtract|multiply|divide|take the remainder of) (.*) and (.*) \(arithmetic needs two numbers\)$)RX", "can't $1 $2 and $3, math only works on two numbers, no cap", "turn both sides into numbers first, with bc::int or bc::float"),
        rule(R"RX(^cannot add (.*) and (.*) \(\+ joins two strings, wrap the other side in bc::str\(\.\.\.\)\)$)RX", "can't add $1 and $2, + only glues two strings together, no cap", "wrap the other side in bc::str(...) first"),
        rule(R"RX(^expected a number but got (.*)$)RX", "that needed a number but got a $1, can't do math on that, no cap", "make sure every value is a number first, bc::float or bc::int can convert text"),
        rule(R"RX(^expected a whole number but got (.*)$)RX", "that needed a whole number but got $1", "pass an int, or use bc::int to turn it into one"),
        rule(R"RX(^bc::sum overflowed the integer range$)RX", "that sum went past the biggest number an int can hold", "convert the values with bc::float first, or sum smaller chunks"),
        rule(R"RX(^tensor sizes cannot be negative$)RX", "a tensor can't have a negative size, that's not a thing", "use 0 or a positive number for rows and columns"),
        rule(R"RX(^bc::randint needs the lower bound to be at most the upper bound$)RX", "randint got its bounds backwards, low has to come first", "swap the two arguments so the smaller one is first"),
        rule(R"RX(^bc::(substr) (start|length) cannot be negative$)RX", "bc::substr can't take a negative $2", "use 0 or a positive number"),
        rule(R"RX(^bc::repeat count cannot be negative$)RX", "can't repeat something a negative number of times", "use 0 or a positive count"),
        rule(R"RX(^bc::argmax of an empty (.*)$)RX", "argmax of an empty $1 has nothing to pick from", "check that it has at least one value first"),
        rule(R"RX(^bc::shape: ragged tensor \(rows of different widths\)$)RX", "that tensor is ragged, its rows have different widths so it has no single shape", "make every row the same length"),
        rule(R"RX(^bc::float: (.*) is too big for a float$)RX", "$1 is too big to fit in a float", "use a smaller number"),
        rule(R"RX(^the parameter '(.*)' is declared twice in func (.*)$)RX", "func $2 lists the parameter '$1' twice, which one is it supposed to be", "rename one of the duplicate parameters"),
        rule(R"RX(^the field '(.*)' is declared twice in class (.*)$)RX", "class $2 lists the field '$1' twice, which one is it supposed to be", "rename one of the duplicate fields"),
        rule(R"RX(^cannot compare (.*) with (.*) using '(.*)' \(ordering needs two numbers or two strings\)$)RX", "can't compare $1 with $2 using '$3', you can only order two numbers or two strings", "make both sides the same kind of thing before comparing"),
        rule(R"RX(^cannot negate a (.*) \(negation needs a number\)$)RX", "can't negate a $1, minus only works on numbers", "only use '-' on numbers"),
        rule(R"RX(^cannot loop over a (.*): 'for x in \.\.\.' needs a vector, array, string or dict \(use 'thru n' to count\)$)RX", "can't loop over a $1, 'for' needs a vector, array, string or dict", "write 'for i in thru n:' if you just want to count"),
        rule(R"RX(^'thru' needs a number of rounds, got (.*)$)RX", "'thru' wants a number of rounds and got a $1 instead", "give it a number, like: for i in thru 5:"),
        rule(R"RX(^'(.*)' takes (.*), but was called with (.*)$)RX", "'$1' wants $2 but got called with $3, the numbers aren't numbering", "call it with exactly the arguments it lists"),
        rule(R"RX(^string index out of bounds$)RX", "that string index is out of range, you went past the end", "check the index against bc::len(text) first"),
        rule(R"RX(^(f64_array|i64_array) index out of bounds$)RX", "that $1 index is out of range, you went past the end", "check the index against bc::len first"),
        rule(R"RX(^(string|vector|f64_array|i64_array) index must be an integer$)RX", "a $1 index has to be an integer", "use a whole number for the index"),
        rule(R"RX(^dict has no key '(.*)'$)RX", "the dict doesn't have a key '$1', nothing there", "check the key spelling, or test with bc::has first"),
        rule(R"RX(^dict keys must be strings$)RX", "dict keys have to be strings", "wrap the key in bc::str(...)"),
        rule(R"RX(^cannot index into (.*)$)RX", "can't index into $1, only vectors, arrays, strings and dicts work with [ ]", "check that the thing before the [ ] is a collection"),
        rule(R"RX(^attempt to call a (.*) value as a function$)RX", "you tried to call a $1 like it's a function, it isn't", "only call names that are functions"),
        rule(R"RX(^maximum recursion depth exceeded \((.*) nested calls\) - this usually means a function is calling itself (.*)$)RX", "recursion went $1 calls deep and hit the limit, a function is probably calling itself forever $2", "add a base case that stops the recursion"),
        rule(R"RX(^out of memory \(the program asked for more memory than is available\)$)RX", "ran out of memory, the program wanted way more than the machine has", "use smaller data, or process it in chunks"),
        rule(R"RX(^string repetition is too large \(limit 1 GiB\)$)RX", "that repeated string would be over 1 GiB, way too big", "repeat it fewer times"),
        rule(R"RX(^vector repetition is too large$)RX", "that repeated vector would be gigantic, way too big", "repeat it fewer times"),
        rule(R"RX(^shape mismatch: (.*)$)RX", "shapes don't match: $1", "make both sides the same length, or use a single number"),
        rule(R"RX(^bc::int cannot convert "(.*)" to an integer$)RX", "bc::int can't turn \"$1\" into a whole number", "make sure the text is only digits, with an optional leading '-'"),
        rule(R"RX(^bc::float cannot convert "(.*)" to a number$)RX", "bc::float can't turn \"$1\" into a number", "make sure the text looks like a number, such as 3.14"),
        rule(R"RX(^unknown builtin '(.*)'$)RX", "'$1' isn't a built-in function", "check the name after bc::"),
        rule(R"RX(^bc::push expects a vector as its first argument \(got (.*)\)$)RX", "bc::push needs a vector first and got a $1", "make sure the first argument is a vector"),
        rule(R"RX(^(bc::[a-z_0-9]+) of an empty (.*)$)RX", "$1 of an empty $2 makes no sense, there's nothing to work with", "check that it has at least one value first"),
        rule(R"RX(^bc::mean of an empty (.*)$)RX", "bc::mean of an empty $1 makes no sense, there's nothing to average", "check that it has at least one value first"),
        rule(R"RX(^read_file: '(.*)' is a directory, not a file$)RX", "'$1' is a folder, not a file, can't read it like one", "point it at a file inside that folder"),
        rule(R"RX(^process::run was cancelled before elevated command execution$)RX", "process::run got cancelled before the elevated command ran, you said no", "run it again and confirm if you really mean it"),
        rule(R"RX(^shell::run was cancelled before elevated command execution$)RX", "shell::run got cancelled before the elevated command ran, you said no", "run it again and confirm if you really mean it"),

        rule(R"RX(^(.*)cannot open '(.*)' \(no such file(?: or unreadable)?\)$)RX", "$1can't open '$2', that file doesn't exist", "check the path and its spelling, and that the file is really there"),
        rule(R"RX(^(.*)cannot open '(.*)' \(permission denied or unreadable\)$)RX", "$1can't open '$2', I'm not allowed to read it", "check the file permissions, or run it as a user that can read the file"),
        rule(R"RX(^(.*)cannot open '(.*)'$)RX", "$1can't open '$2'", "check the path, the spelling and the permissions"),
        rule(R"RX(^(.*)'(.*)' is a directory, not a file$)RX", "$1'$2' is a folder, not a file", "point it at a file inside that folder"),
        rule(R"RX(^(\w+): (.*) is not an open dataset( reader| writer)?$)RX", "$1: $2 isn't an open dataset$3, that handle is dead or never existed", "use the handle you got from open_dataset or create_dataset, and don't use it after close_dataset"),
        rule(R"RX(^(.*) expects a dataset handle as its first argument$)RX", "$1 wants a dataset handle as its first argument", "pass the handle you got from open_dataset or create_dataset"),
        rule(R"RX(^open_dataset: too many datasets are open, close some with close_dataset$)RX", "too many datasets are open at once", "close some with close_dataset before opening more"),
        rule(R"RX(^create_dataset: too many datasets are open, close some with close_dataset$)RX", "too many datasets are open at once", "close some with close_dataset before creating more"),
        rule(R"RX(^bc::format: the template has more placeholders than values$)RX", "bc::format has more {} placeholders than values, one of them is left hanging", "pass one value for every {} in the template"),
        rule(R"RX(^bc::format: (\d+) value\(s\) were not used by the template$)RX", "bc::format got $1 value(s) that no {} used", "add a {} for each value, or drop the extra values"),
        rule(R"RX(^bc::pop cannot take from an empty vector$)RX", "bc::pop can't take from an empty vector, there's nothing in it", "check bc::len(v) > 0 before popping"),
        rule(R"RX(^bc::(pop|insert|remove): index (.*) is out of bounds \(size (.*)\)$)RX", "bc::$1 index $2 is out of range, the size is only $3", "use an index from 0 to size minus 1"),
        rule(R"RX(^bc::remove: key \\?"(.*)\\?" is not in the dict$)RX", "bc::remove can't find the key \"$1\" in the dict", "check bc::has(d, key) first"),
        rule(R"RX(^bc::ord expects a non-empty string$)RX", "bc::ord wants a string with at least one character, this one is empty", "pass a string like \"a\""),
        rule(R"RX(^bc::chr: (.*) is not a valid Unicode code point$)RX", "bc::chr can't make a character from $1, that's not a real Unicode code point", "use a number from 0 to 1114111, not in the 55296 to 57343 range"),
        rule(R"RX(^bc::slice: range \[(.*), (.*)\) is outside a collection of length (.*)$)RX", "that slice from $1 to $2 goes outside a collection of length $3", "keep start and end between 0 and the length"),
        rule(R"RX(^sort\(\) cannot compare numbers with strings - sort numbers and strings separately(.*)$)RX", "bc::sort can't compare numbers with strings, they don't mix$1", "sort the numbers and the strings in separate vectors"),
        rule(R"RX(^dict_merge expects two dicts$)RX", "dict_merge wants two dicts and got something else", "pass two dicts, like bc::dict_merge(a, b)"),
        rule(R"RX(^value is not callable$)RX", "that value isn't callable, you can't call it like a function", "pass a function name, like bc::sqrt"),
        rule(R"RX(^(map|filter|reduce) requires \((.*)\)$)RX", "$1 needs ($2) and didn't get them", "call it like $1($2)"),
        rule(R"RX(^matmul requires two 2D tensors$)RX", "matmul needs two 2D tensors, so two nested vectors", "make both inputs vectors of equal-length vectors"),
        rule(R"RX(^dot: vectors must have the same length$)RX", "dot needs two vectors of the same length", "trim or pad one so the lengths match"),
        rule(R"RX(^f64_array length is too large$)RX", "that f64_array would be way too big", "ask for a smaller length"),
        rule(R"RX(^i64_array length is too large$)RX", "that i64_array would be way too big", "ask for a smaller length"),
        rule(R"RX(^range expects one number: range\(n\) gives 0 to n - 1 \(got (.*) arguments\)$)RX", "range wants one number but got $1 arguments", "write range(n), which counts from 0 up to n minus 1"),
        rule(R"RX(^range expects a number, got (.*)$)RX", "range wants a number and got a $1", "pass a number, like range(10)"),
        rule(R"RX(^bc::int: \\?"(.*)\\?" is outside the integer range$)RX", "bc::int says \"$1\" is too big for an int", "stay within about 9.2 quintillion, or use floats"),
        rule(R"RX(^float is outside the integer range$)RX", "that float is too big to fit in an int", "stay within about 9.2 quintillion, or keep it as a float"),
        rule(R"RX(^bc::(int|float) expects a number, a string or a bool \(got (.*)\)$)RX", "bc::$1 wants a number, a string or a bool and got a $2", "convert it yourself first, or pass one of those types"),
        rule(R"RX(^(f64_array|i64_array) (elements|initial value) must be (.*)$)RX", "$1 $2 need to be $3", "only put $3 into it"),
        rule(R"RX(^process::run could not start '(.*)': (.*)$)RX", "process::run couldn't start '$1': $2", "check the program name, that it's installed, and that it's on your PATH"),
        rule(R"RX(^process::run output exceeded the 64 MiB capture limit$)RX", "that command printed more than 64 MiB, which is the most I'll hold", "use the 'stream' option to pass output through instead of capturing it"),
        rule(R"RX(^(process::run|shell::run) (.*) was not approved$)RX", "$1 wasn't approved, so it didn't run", "run it again and approve it, or build with 'csyc build' first"),
        rule(R"RX(^(.*) was not approved$)RX", "$1 wasn't approved, so it didn't run", "approve it when asked, or run the project through 'csyc build' first"),
        rule(R"RX(^shell::interactive requires an interactive terminal$)RX", "shell::interactive needs a real terminal and this isn't one", "run the program from an interactive terminal, not a pipe or a script"),
        rule(R"RX(^ai::(\w+) expects (.*) to be a f64_array, got (.*)$)RX", "ai::$1 wants $2 to be an f64_array and got $3", "make $2 with bc::f64_array(...)"),
        rule(R"RX(^ai::(\w+) shape mismatch: (.*) has (.*) values, expected (.*)$)RX", "ai::$1 shapes don't line up: $2 has $3 values but needs $4", "check batch, input_width and output_width against the buffer sizes"),
        rule(R"RX(^ai::(\w+) activation must be 'linear', 'relu', 'sigmoid', or 'tanh'$)RX", "ai::$1 doesn't know that activation, it has to be 'linear', 'relu', 'sigmoid' or 'tanh'", "spell the activation exactly as one of those four"),
        rule(R"RX(^ai::(\w+) reduction must be 'sum' or 'mean'$)RX", "ai::$1 reduction has to be 'sum' or 'mean'", "pass 'sum' or 'mean'"),
        rule(R"RX(^ai::(\w+) expects inputs, targets, and each parameter buffer to have independent storage$)RX", "ai::$1 needs every buffer to be its own separate f64_array, two of these share storage", "make a separate f64_array for each one, or copy with bc::copy"),
        rule(R"RX(^invalid output target$)RX", "that output target isn't valid"),
        rule(R"RX(^unknown output sink$)RX", "that output sink isn't a thing"),
        rule(R"RX(^unknown opcode$)RX", "hit an instruction I don't know, the object file is probably damaged or from another version", "rebuild the program with this version of csyc"),
        rule(R"RX(^invalid function index in call_function$)RX", "a call pointed at a function that doesn't exist, the object file is probably damaged", "rebuild the program with this version of csyc"),
        rule(R"RX(^tensor nesting is too deep \(is a vector containing itself\?\)$)RX", "that data is nested way too deep, is a vector inside itself?", "break the loop where a collection holds itself"),
        rule(R"RX(^comparison is nested too deeply \(does a collection contain itself\?\)$)RX", "comparing those went too deep, does a collection hold itself?", "break the loop where a collection holds itself"),
        rule(R"RX(^could not format float$)RX", "couldn't turn that float into text"),
    };
    return table;
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

std::string casualize(std::string text) {
    static const std::pair<const char*, const char*> swaps[] = {
        {"is a directory, not a file", "is a folder, not a file"},
        {"is not supported yet", "isn't supported yet, sorry"},
        {"is not supported", "isn't supported"},
        {"are not supported", "aren't supported"},
        {"unexpected end of input", "the input ended too early"},
        {"unexpected data after", "there's leftover junk after"},
        {"is larger than", "is bigger than"},
        {"larger than", "bigger than"},
        {"out of bounds", "out of range"},
        {"exceeds", "goes over"},
        {"cannot ", "can't "},
        {" does not ", " doesn't "},
        {" do not ", " don't "},
        {" did not ", " didn't "},
        {" is not ", " isn't "},
        {" are not ", " aren't "},
        {" was not ", " wasn't "},
        {" were not ", " weren't "},
        {" must be ", " needs to be "},
        {" must ", " needs to "},
        {" could not ", " couldn't "},
        {" an invalid ", " a busted "},
        {" expects ", " wants "},
        {" requires ", " needs "},
        {"truncated", "cut off"},
        {"mismatch", "mismatch"},
        {"invalid ", "busted "},
    };
    for (const auto& swap : swaps) text = replace_all(text, swap.first, swap.second);
    static const std::regex corrupt(R"RX(\bcorrupt\b)RX");
    return std::regex_replace(text, corrupt, "corrupted");
}

std::string keyword_fix(const std::string& body) {
    struct Hint { const char* needle; const char* fix; };
    static const Hint hints[] = {
        {"is not an open dataset", "use the handle from open_dataset or create_dataset, and not after close_dataset"},
        {"checksum", "the file is damaged, download or generate it again"},
        {"truncated", "the file is cut off, download or generate it again"},
        {"corrupt", "the file is damaged, download or generate it again"},
        {"not a parquet file", "check that it's really a .parquet file and not renamed from something else"},
        {"not a zstd frame", "check that the data really is zstd-compressed"},
        {"unknown dtype code", "the file may be damaged or written by a newer version"},
        {"unknown dtype", "use one of f64, f32, i64, i32 or u8"},
        {"invalid number of dimensions", "the file looks damaged, write it again"},
        {"does not match", "check that the sizes, shape and dtype line up"},
        {"is past the end", "use a smaller offset"},
        {"not supported", "convert the data to a supported kind first, the reference lists what is supported"},
        {"safety limit", "use a smaller input, or process it in chunks"},
        {"too large", "use something smaller, or work in chunks"},
        {"too many", "use fewer, or work in chunks"},
        {"is nested too deeply", "flatten it, or break any loop where something contains itself"},
        {"nested more than", "flatten the data, it's nested too deep"},
        {"must be a vector", "pass a vector, like [1, 2, 3]"},
        {"must be a dict", "pass a dict, like {\"key\": value}"},
        {"must be a non-negative integer", "use a whole number that's 0 or more"},
        {"must be a positive integer", "use a whole number that's 1 or more"},
        {"must be a boolean", "use true or false"},
        {"unknown option", "check the option spelling against the reference"},
        {"is not rectangular", "make every inner vector the same length"},
        {"different number of rows", "make every column the same length"},
        {"has no column", "check the column spelling against the file's header"},
        {"is not in the file", "check the column spelling against the file's header"},
        {"has an empty name", "give that column a name"},
        {"listed twice", "remove the duplicate"},
        {"appears twice", "rename one of the duplicate columns"},
        {"not valid UTF-8", "re-save the text as UTF-8"},
        {"an f64_array or a vector", "pass an f64_array or a vector"},
        {"needs a number", "pass a number"},
    };
    for (const auto& hint : hints)
        if (body.find(hint.needle) != std::string::npos) return hint.fix;
    return "";
}

std::string family_lead(const std::string& name, const std::string& rest) {
    if (name == "parquet") return "that parquet file is giving trouble: ";
    if (name == "zstd") return "the zstd data is busted: ";
    if (name == "decompress") return "decompressing failed: ";
    if (name == "json") return "that JSON is giving trouble: ";
    if (name == "csv") return "that CSV is giving trouble: ";
    static const char* const file_words[] = {"can't open", "cannot open", "directory", "truncated", "corrupt", "checksum",
                                             "reading", "size of", "header", "footer", "file"};
    for (const char* word : file_words)
        if (rest.find(word) != std::string::npos) return name + " couldn't deal with that file: ";
    static const char* const picky_words[] = {"must be", "expects", "needs", "requires", "unknown option", "has to be",
                                              "unsupported", "too large", "too many", "too wide", "is too"};
    for (const char* word : picky_words)
        if (rest.find(word) != std::string::npos) return name + " didn't like that: ";
    return name + " hit a snag: ";
}

std::string json_inner(const std::string& inner) {
    struct Item { const char* from; const char* to; };
    static const Item items[] = {
        {"empty input, expected a JSON value", "there's nothing here, I expected a JSON value"},
        {"unexpected data after the JSON value", "there's extra junk after the JSON value"},
        {"unexpected end of input", "the JSON ends too early, something never got closed"},
        {"invalid literal", "that isn't true, false or null"},
        {"incomplete number", "that number is cut short"},
        {"numbers cannot have leading zeros", "JSON numbers can't start with a 0, like 007"},
        {"invalid number", "that number is busted"},
        {"a digit is required after the decimal point", "there needs to be a digit after the '.'"},
        {"a digit is required in the exponent", "there needs to be a digit in the exponent"},
        {"number is out of range", "that number is too big or too small to hold"},
        {"incomplete \\u escape", "that \\u escape is cut short, it needs 4 hex digits"},
        {"invalid \\u escape", "that \\u escape is busted, it needs 4 hex digits"},
        {"string is not valid UTF-8", "that string isn't valid UTF-8"},
        {"unterminated string", "this string never closes"},
        {"control character in string, it must be escaped", "a raw control character is inside this string, escape it like \\n"},
        {"invalid surrogate pair", "that surrogate pair is busted"},
        {"lone surrogate in \\u escape", "that \\u escape is half of a surrogate pair"},
        {"unterminated array", "this array never closes"},
        {"expected ',' or ']' in array", "I needed ',' or ']' here in the array"},
        {"object keys must be strings", "JSON object keys have to be strings in double quotes"},
        {"expected ':' after object key", "I needed ':' right after the key"},
        {"unterminated object", "this object never closes"},
        {"expected ',' or '}' in object", "I needed ',' or '}' here in the object"},
    };
    for (const auto& item : items)
        if (inner == item.from) return item.to;
    static const std::regex unexpected_char(R"RX(^unexpected character '(.*)'$)RX");
    std::smatch m;
    if (std::regex_match(inner, m, unexpected_char)) return "found '" + m[1].str() + "' where JSON can't have it";
    static const std::regex bad_escape(R"RX(^invalid escape '(.*)'$)RX");
    if (std::regex_match(inner, m, bad_escape)) return "'" + m[1].str() + "' isn't a valid escape in a JSON string";
    return casualize(inner);
}

bool voice_family(const std::string& body, VoicedMessage& out) {
    static const std::regex json_located(R"RX(^json: (.*) \(line ([0-9]+), column ([0-9]+)\)$)RX");
    static const std::regex expects_got(R"RX(^(.+?) expects (.+?) \(got (.+)\)(.*)$)RX");
    static const std::regex expects_plain(R"RX(^(.+?) expects (.+)$)RX");
    static const std::regex needs_plain(R"RX(^(\S+) (?:requires|needs) (.+)$)RX");
    static const std::regex prefixed(R"RX(^([A-Za-z_][A-Za-z_0-9:]*): (.+)$)RX");
    std::smatch m;
    if (std::regex_match(body, m, json_located)) {
        out.text = "that JSON is busted at line " + m[2].str() + ", column " + m[3].str() + ": " + json_inner(m[1].str());
        out.fix = "fix the JSON at that spot, a validator such as 'python3 -m json.tool' points at the same place";
        return true;
    }
    if (std::regex_match(body, m, expects_got)) {
        out.text = m[1].str() + " wants " + casualize(m[2].str()) + " but got " + m[3].str() + m[4].str() + ", that's not it";
        out.fix = "pass " + casualize(m[2].str()) + " there";
        return true;
    }
    if (std::regex_match(body, m, expects_plain)) {
        out.text = m[1].str() + " wants " + casualize(m[2].str());
        out.fix = keyword_fix(body);
        if (out.fix.empty()) out.fix = "check the arguments against its signature in the reference";
        return true;
    }
    if (std::regex_match(body, m, needs_plain)) {
        out.text = m[1].str() + " needs " + casualize(m[2].str());
        out.fix = keyword_fix(body);
        return true;
    }
    if (std::regex_match(body, m, prefixed) && m[1].str().find(' ') == std::string::npos) {
        out.text = family_lead(m[1].str(), m[2].str()) + casualize(m[2].str());
        out.fix = keyword_fix(body);
        return true;
    }
    return false;
}

VoicedMessage voice_body(const std::string& body) {
    if (body.size() > 300) return VoicedMessage{body, ""};
    for (const auto& r : rules()) {
        if (std::regex_match(body, r.pattern)) {
            VoicedMessage out;
            out.text = std::regex_replace(body, r.pattern, r.text);
            if (!r.fix.empty()) out.fix = std::regex_replace(body, r.pattern, r.fix);
            return out;
        }
    }
    VoicedMessage family;
    if (voice_family(body, family)) return family;
    VoicedMessage plain{casualize(body), keyword_fix(body)};
    return plain;
}

}

std::string fix_articles(std::string text) {
    text = replace_all(text, "a f64_array", "an f64_array");
    text = replace_all(text, "a i64_array", "an i64_array");
    return text;
}

VoicedMessage voice_message(const std::string& raw) {
    static const std::string runtime_prefix = "runtime error: ";
    std::string prefix;
    std::string body = raw;
    if (body.compare(0, runtime_prefix.size(), runtime_prefix) == 0) {
        prefix = runtime_prefix;
        body = body.substr(runtime_prefix.size());
    }
    std::string suffix;
    static const std::regex context(R"RX(^(.*?)( \((at top level|in function '[^']*')(, line [0-9]+)?\))$)RX");
    std::smatch m;
    if (std::regex_match(body, m, context)) {
        const std::string where = m[3].str();
        const std::string line = m[4].str();
        suffix = where == "at top level" ? " (top level" + line + ")"
                                         : " (inside " + where.substr(std::string("in function ").size()) + line + ")";
        body = m[1].str();
    }
    VoicedMessage voiced = voice_body(body);
    voiced.text = fix_articles(prefix + voiced.text + suffix);
    voiced.fix = fix_articles(voiced.fix);
    return voiced;
}

std::string voice_text(const std::string& raw) {
    static const std::regex did_you_mean(R"RX(^did you mean (.*)\?$)RX");
    if (std::regex_match(raw, did_you_mean)) return std::regex_replace(raw, did_you_mean, "maybe you meant $1?");
    return voice_body(raw).text;
}

std::string voice_footer(std::size_t errors, std::size_t warnings) {
    const char* lead = errors == 0 ? "not broken, just sus: " : errors <= 3 ? "small L, easy fix: " : "big L, take it one at a time: ";
    return std::string(lead) + std::to_string(errors) + " error(s), " + std::to_string(warnings) + " warning(s)";
}

}
