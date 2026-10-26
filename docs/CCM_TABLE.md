# Cooler Compiler(CCM):

You WANT to customize your compiler?
This .md is for YOU!

### Two files, two jobs

- `ccm_table.bin` is built from THIS file It holds your words. It never changes while you compile.
- `ccm_mem.bin` is written by the compiler as you work. It holds what happened (records), not words. This is what `csyc prune --ccm-mem` and `csyc wipe --ccm-mem` act on.

### Fields you can use

# Field
`{file}`    : the path of the source file the compiler was given.
`{name}`    : the name involved when the message is about one (a variable, a function, a module, what was expected)
`{line}`    : the line number where it happened
`{col}`     : the column where it happened.
`{count}`   : a number whose meaning is given in each situation (errors, warnings, failures in a row, exit code ...)
`{time}`    : how long something took, or how long since something happened
`{size}`    : the size of a file or of the output
`{limit}`   : the limit that was hit.
`{percent}` : how much of the memory budget is used.
`{error}`   : the short name of an error situation from Part A (used in the memory part).

A `count=...` or `name=...` or `size=...` note in a situation tells you what that field means there.

# Part A: what the compiler says

## A1  How a build or a run ended

### compile.success
**Compile success**  
when: a build finished and the object file was written (no warnings)  
fields: {file}, {time}, {size}

- Hell yeah {file} came out clean in {time}
- Look at that {file} actually behaved
- Clean win on {file} no drama
- {file} just vibed its way through in {time}
- Proud of this one {file} size {size}
- Alright {file} is looking sharp
- Nice one {file} you did not even fight me
- {file} compiled clean somehow
- Solid work on {file} took {time}
- {file} is good to go and I am lowkey happy
- That is a proper clean compile on {file}
- {file} did not try any nonsense
- Smooth as butter {file} in {time}
- {file} is solid and I will admit it
- Easy win for {file} size {size}
- {file} just works
- Clean and quiet on {file}
- {file} behaved and I am not even mad
- Perfect little compile on {file}
- {file} came through clean in {time}
- Another clean one for {file}
- {file} is good size {size}
- You got a real clean one on {file}
- {file} did not even try to mess with me
- That compile on {file} felt good
- gg fricking easy! (compile success)

### compile.success.warnings
**Compile success with warnings**  
when: a build finished and the object file was written, but warnings were printed  
fields: {file}, {time}, {size}, {count} (warnings)

- {file} compiled but those {count} warnings are still staring at you
- It works if you ignore the {count} little problems
- Success with {count} warnings on {file}
- {file} made it through with {count} warnings still alive
- Yeah it compiled but those {count} warnings did not vanish
- {count} warnings and a success on {file}
- It runs but {count} warnings came along for the ride
- {file} is usable with {count} warnings attached
- Success plus {count} warnings on {file}
- {file} compiled with {count} reminders hanging around
- Those {count} warnings did not stop {file}
- {file} works and still has {count} warnings watching
- {count} warnings survived the compile of {file}
- {file} is done but not quiet {count} warnings left
- You got the binary and {count} warnings
- {file} succeeded with {count} side notes
- Warnings {count} still present on {file}
- {file} is good enough with {count} warnings
- {count} warnings tag along with {file}
- Success on {file} despite {count} warnings
- {file} compiled and kept {count} warnings like a souvenir
- Those {count} warnings are still there on {file}
- {file} works with {count} warnings in tow
- {count} warnings did not kill the compile of {file}
- Mixed result on {file} success and {count} warnings

### run.success
**Program finished**  
when: a program run ended normally (the compiler prints that it finished successfully)  
fields: {file}, {time}

- {file} finished clean in {time}
- Program done {file} took {time}
- Nice run {file} no crashes
- {file} made it all the way through
- Smooth finish on {file} in {time}
- Alright {file} is done running
- {file} completed without drama
- Clean exit from {file}
- {file} ran its course in {time}
- No issues on the run of {file}
- {file} finished like it should
- Solid run on {file}
- {file} is done and dusted
- Program closed clean on {file}
- {file} exited normally
- Good run for {file} in {time}
- {file} behaved during the run
- Finished without trouble {file}
- {file} ran fine
- Clean completion for {file}
- {file} did its job
- Run success on {file}
- {file} wrapped up nicely
- No surprises from {file}
- {file} finished strong in {time}

### run.exit.nonzero
**Program ended with an error exit code**  
when: a program run ended with a non zero exit code  
fields: {file}, {count} (exit code)

- {file} quit with exit code {count}
- Yeah that run died exit {count} on {file}
- {file} just walked out with code {count}
- Exit code {count} from {file} not great
- {file} failed the run with {count}
- Program bailed exit {count} on {file}
- Non zero exit {count} from {file}
- {file} returned {count} and left
- Exit status {count} on {file}
- {file} ended with code {count}
- Bad exit code {count} from {file}
- {file} did not exit clean {count}
- Code {count} came back from {file}
- {file} stopped with {count}
- Nonzero from {file} code {count}
- {file} gave exit {count}
- Run failed exit {count} on {file}
- {file} returned error code {count}
- Exit {count} is what {file} left us with
- {file} closed with {count}
- Code {count} from the run of {file}
- {file} exited poorly with {count}
- Bad return code {count} on {file}
- {file} ended badly exit {count}
- Non zero {count} from {file} 

### run.rebuilt
**Rebuilt before running**  
when: csyc run found the object out of date and rebuilt it first  
fields: {file}

- Had to rebuild {file} first before running
- {file} was stale so I fixed that for you
- Rebuilt {file} real quick then ran it
- Object was old so {file} got a fresh compile
- Touched {file} up before the run
- {file} needed a rebuild and now it is ready
- Stale object on {file} so I handled it
- {file} was out of date I fixed it
- Quick rebuild of {file} before the run
- {file} got refreshed first
- Had to recompile {file} to run it
- Object for {file} was outdated
- Rebuilt {file} on the way
- {file} needed a fresh object
- Compiled {file} again before running
- {file} was not current so I caught it up
- Rebuild happened for {file}
- {file} got a new object first
- Stale binary for {file} corrected
- {file} rebuilt and ready
- Forced a rebuild of {file}
- {file} was behind so I fixed it
- Object refresh on {file}
- {file} needed updating first
- Recompiled {file} for the run

### run.interrupted
**Stopped with Ctrl C**  
when: you interrupted a build or a run  
fields: {file}, {time}

- You hit stop on {file} after {time}
- Interrupted {file} at {time}
- Ctrl C on {file} got it
- {file} stopped early after {time}
- You cancelled {file} mid way
- {file} got cut off at {time}
- Stop signal on {file} after {time}
- {file} was interrupted
- You pulled the plug on {file}
- {file} did not finish you stopped it
- Cancelled at {time} on {file}
- {file} got interrupted after {time}
- Manual stop on {file}
- {file} halted by you
- You ended {file} early
- Interrupt received for {file}
- {file} stopped by user
- Early exit on {file} after {time}
- {file} did not complete
- You cut {file} short
- Stopped {file} at {time}
- {file} interrupted mid process
- User stop on {file}
- {file} cancelled after {time}
- Manual interrupt of {file}

## A2  Reading the source file

### file.unreadable
**Source file cannot be read**  
when: the file is missing or cannot be opened  
fields: {file}

- Cannot even open {file} what happened
- {file} is missing or locked somehow
- Yeah I cannot read {file} at all
- {file} is not cooperating
- Cannot find or open {file}
- {file} is unreachable right now
- No access to {file}
- {file} will not open
- Missing or blocked {file}
- Cannot load {file}
- {file} is not available
- Unable to read {file}
- {file} refuses to open
- Access denied or missing for {file}
- {file} is gone or locked
- Cannot get into {file}
- {file} is unreadable
- No luck opening {file}
- {file} is not there or blocked
- Failed to open {file}
- {file} cannot be accessed
- Source {file} is unavailable
- {file} is out of reach
- Cannot touch {file}
- {file} is missing in action
- WHERE IS THE {file}

### file.too.big
**Source file over the 32 MiB safety limit**  
when: a source file is larger than the hard limit  
fields: {file}, {size}, {limit}

- {file} is {size} and the limit is {limit}
- That file is way too big {size} over {limit}
- {file} crossed the {limit} line at {size}
- Too huge {file} size {size} limit {limit}
- {file} is oversized {size} versus {limit}
- Cannot take {file} it is past {limit}
- Size {size} exceeds limit {limit} for {file}
- {file} is larger than allowed {size}
- Over the hard limit with {file}
- {file} at {size} is too much
- Limit {limit} broken by {file} at {size}
- {file} is beyond {limit}
- Too big to handle {file}
- {file} size {size} is over the line
- Hard limit hit by {file}
- {file} is too large at {size}
- Cannot process {file} past {limit}
- Size limit violated by {file}
- {file} weighs too much {size}
- Oversize source {file}
- {file} exceeds safety size
- Limit {limit} vs actual {size} on {file}
- {file} is oversized for safety
- Too large file {file}
- {file} goes past {limit}

### file.changed.while.reading
**File changed while it was being read**  
when: the source changed or could not be read completely  
fields: {file}

- {file} changed while I was reading it
- Something touched {file} mid read
- {file} got modified during the load
- Cannot trust {file} it shifted under me
- {file} was not stable while reading
- File changed on me {file}
- {file} moved during the read
- Inconsistent read of {file}
- {file} was altered mid process
- Source {file} changed underneath
- Unstable file {file}
- {file} did not stay still
- Read interrupted by change in {file}
- {file} was edited while loading
- Change detected in {file} during read
- {file} is not consistent
- Mid read modification of {file}
- {file} shifted while open
- Cannot finish reading {file}
- {file} changed on disk
- Live change in {file}
- {file} was not static
- Read failed due to change in {file}
- {file} got updated mid way
- Unreliable source {file}
- Bro why is {file} doesn't look the same

### syntax.expected
**Something expected is missing**  
when: the parser expected something and found something else (expected a name after func, expected as after using, and so on)  
fields: {file}, {line}, {col}, {name} (what was expected)

- Expected {name} at line {line} col {col} in {file}
- Missing {name} right there line {line} of {file}
- {file} line {line} wanted {name} and did not get it
- Look at line {line} in {file} it needs {name}
- Parser wanted {name} at {line}:{col} in {file}
- {name} is missing on line {line} of {file}
- {file} expected {name} at line {line}
- No {name} where it should be in {file}
- Missing piece {name} at {line}:{col}
- {file} needs {name} on line {line}
- Expected token {name} not found
- {name} should be at line {line} in {file}
- Parser looking for {name} in {file}
- {file} is missing {name} at that spot
- Line {line} of {file} lacks {name}
- {name} was expected and absent
- {file} syntax wants {name}
- No {name} at the expected place
- {name} is required at line {line}
- Missing {name} in {file}
- {file} at line {line} needs {name}
- Expected {name} did not appear
- {name} gap at {line}:{col} in {file}
- {file} is short one {name}
- Wanted {name} got something else

### syntax.elif
**Wrote elif**  
when: the compiler noticed elif and knows CSy uses orf  
fields: {file}, {line}, {col}

- That is elif not orf line {line} in {file}
- You wrote elif at {line}:{col} in {file}
- {file} line {line} has elif try orf instead
- Elif spotted in {file} at line {line}
- Wrong keyword elif on line {line} of {file}
- {file} still using elif at {line}:{col}
- Elif is not the word here {file}
- Use orf not elif in {file}
- {file} has elif which is wrong
- Elif detected at line {line}
- That should be orf not elif
- {file} line {line} says elif
- Wrong branch keyword in {file}
- Elif does not belong in {file}
- Change elif to orf at line {line}
- {file} used elif by mistake
- Elif found where orf belongs
- {file} has the old elif style
- Keyword elif is incorrect here
- {file} needs orf not elif
- Elif at {line}:{col} in {file}
- That elif should be orf
- {file} is using elif still
- Fix the elif on line {line}
- Orf is the correct word not elif

### syntax.stray.ext
**A ::ext with nothing to close**  
when: a ::ext appeared where no block was open  
fields: {file}, {line}, {col}

- Stray ::ext at line {line} in {file}
- Nothing open but ::ext showed up in {file}
- {file} line {line} has a lonely ::ext
- Extra ::ext floating at {line}:{col} in {file}
- That ::ext has nowhere to go in {file}
- Unmatched ::ext on line {line} of {file}
- Lonely ::ext in {file}
- ::ext without a block in {file}
- Stray closer at line {line}
- {file} has unmatched ::ext
- No open block for that ::ext
- Extra ::ext in {file} at {line}
- ::ext appeared alone
- {file} closed nothing with ::ext
- Unmatched extension marker
- ::ext is out of place in {file}
- No block to close at line {line}
- Stray ::ext detected
- {file} has a floating ::ext
- ::ext with no pair
- Lonely closer in {file}
- {file} line {line} ::ext is extra
- Nothing to match that ::ext
- Unbalanced ::ext in {file}
- Extra end marker at {line}

### syntax.unterminated.string
**String never closed**  
when: a string literal has no closing quote  
fields: {file}, {line}, {col}

- String never closed in {file} line {line}
- Missing quote at line {line} of {file}
- {file} has an open string at {line}:{col}
- That string keeps going forever in {file}
- Unclosed string on line {line} in {file}
- Quote problem at {line}:{col} in {file}
- Open string in {file}
- No closing quote found
- String runs away in {file}
- {file} string is not finished
- Missing end quote at line {line}
- Unterminated string literal
- {file} left a string open
- Quote never closed in {file}
- String starts but does not end
- Open quote at {line}:{col}
- {file} has unfinished string
- No matching quote found
- String leaks past line {line}
- Unclosed quote in {file}
- {file} string is incomplete
- Missing closer for the string
- String open ended in {file}
- Quote imbalance at line {line}
- Unfinished string literal

### syntax.single.and.or
**Wrote & or | where && or || is meant**  
when: the lexer found a single & or | in an expression  
fields: {file}, {line}, {col}

- Single operator instead of double at line {line} in {file}
- You need the double version line {line} of {file}
- {file} line {line} has a lonely operator
- Bitwise looking thing at {line}:{col} in {file}
- Wrong operator style on line {line} of {file}
- Single operator spotted in {file} at {line}
- Use the double form not single
- {file} has single where double belongs
- Operator should be doubled at line {line}
- Single form found in {file}
- Wrong length operator in {file}
- Double it up at line {line}
- {file} used single operator
- Logical operator needs two chars
- Single form at {line}:{col}
- {file} has short operator
- Need the double form not the single
- Operator too short in {file}
- Single version used by mistake
- {file} line {line} operator is single
- Double the operator please
- Short operator detected
- {file} needs the long form
- Single char operator wrong here
- Use full logical operator

### syntax.too.deep
**Nested or chained too deep**  
when: nesting passed 256 levels, or an expression is too long or too deeply chained  
fields: {file}, {line}, {col}, {limit}

- Nesting hit the {limit} wall in {file}
- Too deep at line {line} of {file} limit {limit}
- {file} went past {limit} levels of nesting
- Expression too thick in {file} at {line}
- Depth limit {limit} broken in {file}
- {file} nested itself into trouble at {line}
- Nesting too far in {file}
- {limit} levels is the max
- {file} is nested past safety
- Too much depth at line {line}
- Expression chain too long
- {file} hit nesting limit {limit}
- Deep nesting problem in {file}
- Limit {limit} exceeded
- {file} went too deep
- Nesting overflow at {line}
- Structure too nested in {file}
- Depth safety triggered
- {file} nesting is excessive
- Too many layers in {file}
- Nesting limit reached
- {file} is over nested
- Chain too long at line {line}
- Depth {limit} broken
- {file} nested beyond limit

### syntax.too.many.errors
**Too many errors in one file**  
when: the compiler stopped reading a file because of the number of errors  
fields: {file}, {count}

- Stopped at {count} errors in {file}
- {file} has too many problems already {count}
- Gave up after {count} errors in {file}
- {count} errors is enough for {file}
- {file} is a mess with {count} errors
- Cutting off {file} after {count} issues
- Error limit hit in {file}
- {count} errors and done
- {file} has excessive errors
- Too many to continue on {file}
- Stopped reading {file} at {count}
- {count} is past the error budget
- {file} error count too high
- Giving up on {file}
- Error flood in {file}
- {count} errors stopped the parse
- {file} is too broken
- Max errors reached on {file}
- {file} has {count} and counting stopped
- Error overflow in {file}
- Parse halted on {file}
- {count} errors is the limit
- {file} too error heavy
- Stopped after {count} in {file}
- Error budget spent on {file}

### literal.overflow
**Integer literal too big**  
when: an integer written in the code does not fit  
fields: {file}, {line}, {col}

- Number too big at line {line} in {file}
- That literal overflows in {file} line {line}
- {file} has an oversized number at {line}:{col}
- Integer too large on line {line} of {file}
- Overflow risk at {line}:{col} in {file}
- Number does not fit in {file} at line {line}
- Literal too large in {file}
- Integer overflow in source
- Number exceeds limit at line {line}
- {file} has huge literal
- Too big integer at {line}:{col}
- Literal will not fit
- Oversized number in {file}
- Integer too wide at line {line}
- {file} literal is excessive
- Number out of range
- Big literal problem in {file}
- Integer size exceeded
- {file} has out of bounds number
- Literal overflow at line {line}
- Number too large for the type
- {file} line {line} number is huge
- Overflowing literal found
- Integer does not fit
- Too big for the integer

### syntax.other
**Any other syntax error**  
when: a syntax error that has no situation of its own above  
fields: {file}, {line}, {col}

- Syntax problem at line {line} in {file}
- Something is off in {file} around {line}:{col}
- {file} has a syntax issue on line {line}
- Parser tripped at {line}:{col} in {file}
- Odd syntax in {file} line {line}
- {file} is not parsing clean at line {line}
- Syntax error in {file}
- Parse failed at line {line}
- {file} syntax is broken there
- Unexpected syntax at {line}:{col}
- {file} has bad syntax
- Parser error on line {line}
- Syntax mess in {file}
- {file} line {line} is invalid
- Bad form at {line}:{col}
- Syntax does not check out
- {file} failed to parse cleanly
- Issue in the syntax of {file}
- Parse problem at line {line}
- {file} has structural error
- Syntax fault in {file}
- Unexpected structure at line {line}
- {file} is syntactically off
- Bad syntax found
- Parse rejected that part of {file}

## A3  Connecting files, modules and projects

### connect.file.missing
**Connected file cannot be read**  
when: #connect points at a file that is missing or unreadable  
fields: {file}, {name} (the path in #connect)

- Cannot find the connected file {name} from {file}
- {name} is missing for the connect in {file}
- Connected path {name} is unreachable
- {file} wants {name} but it is not there
- No luck reading {name} from {file}
- Connect target {name} failed in {file}
- Missing connect file {name}
- {name} not found for {file}
- Cannot open connected {name}
- {file} connect to {name} failed
- {name} is absent
- Connect lookup for {name} failed
- {file} cannot reach {name}
- Linked file {name} missing
- {name} does not exist
- Failed to load {name}
- {file} pointed at missing {name}
- Connect source {name} gone
- Unable to read {name}
- {name} unavailable for connect
- {file} needs {name} which is missing
- No such connected file {name}
- {name} cannot be opened
- Connect failed on {name}
- Missing dependency {name}

### connect.wrong.extension
**Connected file is not a .csy file**  
when: #connect points at a file with the wrong extension  
fields: {file}, {name}

- {name} is not a csy file from {file}
- Wrong extension on {name} in {file}
- Connect wants a csy file not {name}
- {file} pointed at non csy {name}
- That file {name} has the wrong type
- {name} extension is off for {file}
- Incorrect file type for connect
- {name} is not csy
- Wrong kind of file {name}
- {file} connected a non csy
- Extension mismatch on {name}
- {name} should be csy
- Invalid extension for connect
- {file} used wrong file type
- {name} is the wrong format
- Connect expects csy got something else
- {name} type is incorrect
- Non csy file in connect
- {file} has bad connect target type
- Extension error on {name}
- {name} is not acceptable
- Wrong file class for {name}
- {file} connect type mismatch
- {name} fails extension check
- Only csy files allowed

### connect.unknown.export
**Asked for a name the file does not export**  
when: the #connect allowlist names something that is not there  
fields: {file}, {name}

- {name} is not exported for {file}
- No such export {name} in the connect
- {file} asked for {name} which is missing
- Unknown export {name} from {file}
- That name {name} is not available
- Connect cannot find export {name}
- {name} not in the export list
- Missing export {name}
- {file} wants unknown {name}
- Export {name} does not exist
- No export named {name}
- {name} is not provided
- Unknown name in connect {name}
- {file} requested missing {name}
- Export lookup failed for {name}
- {name} is not exported
- Cannot import {name}
- {file} has invalid export request
- {name} not found in exports
- Bad export name {name}
- {name} is not offered
- Export {name} missing
- {file} asked for non export
- No match for {name}
- Unknown {name} in allowlist

### connect.interface.not.implemented
**Interface has no implementation**  
when: a connected interface was not matched by an implementation  
fields: {file}, {name} (interface)

- Interface {name} has no implementation in {file}
- {name} is declared but never implemented
- Missing implementation for {name}
- {file} left interface {name} hanging
- No body for interface {name}
- {name} interface is incomplete
- Unimplemented interface {name}
- {name} has no matching body
- Interface {name} not filled
- {file} missing impl for {name}
- No implementation of {name}
- {name} is only an interface
- Incomplete {name} in {file}
- Implementation gap for {name}
- {name} needs a body
- Interface without code {name}
- {file} did not implement {name}
- Missing code for {name}
- {name} is abstract still
- No concrete version of {name}
- Interface {name} unmatched
- {file} forgot the impl
- {name} remains unimplemented
- Gap in interface {name}
- {name} has no realization

### connect.too.deep
**Files connected too deeply**  
when: connections nest past the 256 file safety limit  
fields: {file}, {limit}

- Connect depth hit {limit} from {file}
- Too many nested connects limit {limit}
- {file} connected past the {limit} line
- Nesting of files went too far
- Connect chain too long limit {limit}
- Depth limit {limit} broken by connects
- File nesting too deep
- {limit} connects is the max
- {file} went too far with connects
- Connect depth exceeded
- Too many layers of connect
- Safety limit {limit} hit
- Nested connects overflow
- {file} connect tree is too tall
- Depth safety on connects
- {limit} levels of connect broken
- Connect recursion too deep
- {file} nested connects excessively
- Max connect depth reached
- Too deep connect chain
- {file} over nested the connects
- Connect limit {limit} passed
- Nesting overflow on connects
- {file} has excessive connect depth
- Depth wall hit at {limit}

### project.file.unreadable
**Project file cannot be read**  
when: project.csp is missing or unreadable  
fields: {file}

- Cannot read the project file {file}
- Project file is missing or locked
- {file} is not readable as project
- No luck opening project {file}
- Project file {file} is unreachable
- Cannot load project from {file}
- Missing project file {file}
- {file} project cannot open
- Project config unreadable
- {file} is not accessible
- Cannot access project {file}
- Project file blocked or gone
- {file} fails to load as project
- No project file found
- {file} is missing for project
- Unable to read project config
- Project {file} unavailable
- {file} cannot be used as project
- Project load failed
- {file} is locked or absent
- Cannot open the project
- Project file problem with {file}
- {file} is not loadable
- Missing or bad project file
- Project config out of reach

### project.file.too.big
**Project file over 1 MiB**  
when: project.csp is bigger than the configuration limit  
fields: {file}, {size}, {limit}

- Project file {file} is {size} over {limit}
- {file} is too big for a project file
- Size {size} exceeds project limit {limit}
- Project config {file} is oversized
- {file} crossed the project size line
- Too large project file at {size}
- Project size limit broken
- {file} is larger than allowed
- Config file too heavy
- {size} is past project limit
- {file} exceeds config size
- Project file oversized
- Limit {limit} vs {size} on project
- {file} is too big for config
- Project config too large
- Size safety hit on project
- {file} project file is huge
- Oversize project config
- {file} past the size limit
- Project file weight problem
- {size} too much for project
- Config size exceeded
- {file} is over the project limit
- Too big to be project config
- Project file size violation

### module.unknown
**Unknown module**  
when: #inc names a module the compiler does not know  
fields: {file}, {line}, {col}, {name} (module)

- Unknown module {name} at line {line} in {file}
- No such module {name} in {file}
- {file} asked for module {name} which is missing
- Module {name} not found line {line}
- {name} is not a known module
- Cannot find module {name} in {file}
- Missing module {name}
- {name} module does not exist
- Unknown include {name}
- {file} wants unknown module
- No module named {name}
- Module lookup failed for {name}
- {name} is not available
- {file} has bad module name
- Unknown {name} module
- Module {name} missing
- Cannot load module {name}
- {file} line {line} unknown module
- No such {name} to include
- Module {name} not recognized
- {name} is invalid module
- Include failed for {name}
- {file} referenced missing module
- Unknown module name {name}
- Module not found {name}

## A4  Checking the program

### name.undefined
**Name that was never defined**  
when: a variable or function name is used but not defined  
fields: {file}, {line}, {col}, {name}

- {name} is not defined at line {line} in {file}
- Never saw {name} before in {file}
- Undefined name {name} on line {line}
- {file} uses {name} but it does not exist
- Where is {name} supposed to come from
- {name} is missing in {file} at {line}
- Unknown name {name}
- {name} was never declared
- {file} has undefined {name}
- No definition for {name}
- {name} does not exist
- Undefined identifier {name}
- {file} line {line} unknown {name}
- {name} is not in scope
- Missing definition of {name}
- {name} used but not made
- No such name {name}
- {file} references unknown {name}
- {name} is undeclared
- Cannot find {name}
- {name} has no origin
- Undefined at line {line}
- {file} lacks {name}
- Name {name} unknown
- {name} not found

### name.duplicate
**Name defined twice**  
when: a name is declared more than once in the same place  
fields: {file}, {line}, {col}, {name}

- {name} is defined more than once in {file}
- Duplicate name {name} at line {line}
- {file} already has a {name}
- Name collision on {name} in {file}
- {name} got declared twice
- Double definition of {name} at {line}
- {name} is repeated
- Duplicate {name} found
- {file} redefines {name}
- Name {name} already used
- Collision on {name}
- {name} declared again
- Second definition of {name}
- {file} has two {name}
- Duplicate identifier {name}
- {name} appears twice
- Redefinition of {name}
- {file} line {line} duplicate {name}
- Name clash with {name}
- {name} is not unique
- Multiple {name} definitions
- {file} redeclared {name}
- Duplicate found for {name}
- {name} already exists
- Second {name} is invalid

### call.wrong.arguments
**Wrong number of arguments**  
when: a call passes too many or too few arguments  
fields: {file}, {line}, {col}, {name} (function), {count} (how many were given)

- {name} got {count} arguments at line {line} in {file}
- Wrong argument count for {name} in {file}
- {file} called {name} with {count} args
- Argument mismatch on {name} line {line}
- {name} does not take {count} arguments
- Call to {name} has the wrong count
- Bad arity for {name}
- {name} argument count wrong
- {count} args given to {name}
- {file} has wrong call to {name}
- Argument number mismatch
- {name} expects different count
- Wrong number of params for {name}
- {file} line {line} bad call
- {name} called incorrectly
- Arity error on {name}
- {count} is wrong for {name}
- Call arguments do not match
- {name} parameter count off
- Bad call site for {name}
- {file} mismatched {name}
- Argument count error
- {name} got unexpected count
- Wrong args to {name}
- Call arity problem

### check.other
**Any other error found while checking**  
when: a checking error that has no situation of its own above  
fields: {file}, {line}, {col}

- Check failed at line {line} in {file}
- Something wrong in {file} around {line}:{col}
- {file} has a checking issue on line {line}
- Semantic problem at {line}:{col} in {file}
- Check trip at line {line} of {file}
- {file} did not pass checking cleanly
- Checking error in {file}
- Semantic error at line {line}
- {file} failed a check
- Problem found during check
- {file} has a semantic issue
- Check rejected that part
- Error during checking of {file}
- {file} line {line} check fail
- Semantic fault in {file}
- Checking did not like that
- {file} has an issue at {line}
- Failed semantic check
- Problem at {line}:{col}
- {file} did not check out
- Checking error found
- {file} has a deep issue
- Semantic problem detected
- Check failed on {file}
- Issue in program check

## A5  Warnings

### warn.hides.builtin
**Name hides a built in**  
when: a variable has the same name as a built in function such as sum or max  
fields: {file}, {line}, {col}, {name}

- {name} hides a built in at line {line} in {file}
- You shadowed {name} which is a built in
- Built in name {name} got covered in {file}
- {file} reuses built in name {name}
- Careful {name} is already a built in
- Shadowing built in {name} on line {line}
- {name} covers a built in
- Built in {name} is hidden
- {file} shadows {name}
- Name clash with built in {name}
- {name} is a built in name
- Shadowing detected for {name}
- {file} hides built in {name}
- Built in covered by {name}
- {name} overrides built in
- Caution on {name} name
- Built in shadow at line {line}
- {file} uses built in name
- {name} is reserved in a way
- Shadow of built in {name}
- {file} line {line} hides {name}
- Built in name reused
- {name} collides with built in
- Hiding built in with {name}
- Name {name} is special

### warn.constant.condition
**Condition is always true or false**  
when: an if or while condition is a constant  
fields: {file}, {line}, {col}

- Constant condition at line {line} in {file}
- That if or while never changes in {file}
- Always true or false at {line}:{col}
- {file} has a fixed condition on line {line}
- Condition is constant in {file}
- Pointless condition check at line {line}
- Constant test in {file}
- Condition never varies
- {file} has static condition
- Always the same result
- Fixed condition at line {line}
- {file} condition is constant
- No variation in the test
- Constant if or while
- {file} line {line} fixed test
- Condition cannot change
- Static boolean in {file}
- Always true or always false
- Pointless branch condition
- {file} has constant logic
- Fixed value condition
- Condition is not dynamic
- {file} test is constant
- Unchanging condition
- Constant expression used

### warn.unreachable
**Code that can never run**  
when: a statement comes after something that always leaves the block  
fields: {file}, {line}, {col}

- Unreachable code at line {line} in {file}
- That part never runs in {file}
- Dead code spotted at {line}:{col}
- {file} has code after an exit on line {line}
- Nothing reaches line {line} in {file}
- Unreachable statement in {file}
- Dead code in {file}
- Code after exit at line {line}
- {file} has unreachable part
- That statement is never hit
- Unreachable at {line}:{col}
- {file} line {line} is dead
- Code cannot be reached
- After exit code found
- {file} has dead section
- Unreachable block
- Statement after return like
- {file} has code past the end
- Dead path at line {line}
- Nothing executes that
- Unreachable in {file}
- Code after terminal statement
- {file} line {line} never runs
- Dead code detected
- Unreachable fragment

### warn.self.assign
**Variable assigned to itself**  
when: a statement like x = x that does nothing  
fields: {file}, {line}, {col}, {name}

- {name} assigned to itself at line {line} in {file}
- Self assignment on {name} does nothing
- {file} has {name} equals {name}
- Pointless self assign of {name}
- {name} set to itself on line {line}
- That assignment changes nothing for {name}
- Self assign of {name}
- {name} to {name} is useless
- No effect assignment
- {file} self assigns {name}
- Pointless {name} equals {name}
- Self write to {name}
- {name} assigned to same
- No change from that assign
- {file} line {line} self assign
- Useless assignment of {name}
- {name} gets itself
- Self assignment detected
- {file} has no op assign
- {name} equals {name} found
- Assignment with no effect
- Self target on {name}
- {file} did nothing with {name}
- Pointless write to {name}
- No op on {name}

### warn.stop.skip.outside.loop
**stop or skip outside a loop**  
when: stop or skip is used where there is no loop  
fields: {file}, {line}, {col}, {name}

- {name} outside any loop at line {line} in {file}
- No loop for that {name} in {file}
- {file} used {name} in the wrong place
- {name} only works inside loops
- Stray {name} on line {line} of {file}
- {name} has no loop to affect
- {name} outside loop
- Misplaced {name}
- {file} has {name} without loop
- No enclosing loop for {name}
- {name} in wrong context
- Loop control outside loop
- {file} line {line} bad {name}
- {name} needs a loop
- Stray loop control
- {name} has no target loop
- Outside loop use of {name}
- {file} misplaced {name}
- Invalid {name} placement
- {name} not in loop
- Loop keyword out of place
- {file} has free {name}
- {name} without loop
- Bad location for {name}
- {name} is loop only

### warn.loop.variable.assigned
**Assigning to a loop variable**  
when: the loop variable is assigned inside its own loop  
fields: {file}, {line}, {col}, {name}

- Assigning to loop variable {name} at line {line}
- {name} is the loop var and got changed
- {file} modifies its own loop variable {name}
- Careful with {name} inside its loop
- Loop variable {name} got written to
- Changing {name} mid loop in {file}
- Loop var {name} assigned
- Writing to {name} in its loop
- {file} changes loop {name}
- {name} modified inside loop
- Assignment to loop control
- {name} is loop variable
- Changing the loop var
- {file} writes to {name}
- Loop variable mutation
- {name} gets new value in loop
- Modifying {name} during loop
- {file} line {line} changes {name}
- Loop var assignment
- {name} is being overwritten
- Assignment inside own loop
- {file} alters {name}
- Loop control changed
- {name} reassigned in loop
- Changing iteration variable

### warn.deprecated
**Deprecated feature**  
when: something deprecated is used, for example bc matmul  
fields: {file}, {line}, {col}, {name}

- Deprecated {name} at line {line} in {file}
- {name} is old and not preferred
- {file} still uses deprecated {name}
- That feature {name} is on the way out
- Deprecated call to {name} on line {line}
- {name} is marked deprecated in {file}
- {name} is deprecated
- Old feature {name} used
- {file} has deprecated {name}
- Prefer something else over {name}
- {name} will go away
- Deprecated usage of {name}
- {file} line {line} uses old {name}
- {name} is outdated
- Avoid {name} if possible
- Deprecated in {file}
- {name} is legacy
- Old style {name}
- {file} relies on deprecated
- {name} marked for removal
- Use of deprecated {name}
- {file} has old feature
- {name} is not current
- Deprecated path taken
- Prefer newer than {name}

### warn.duplicate.key
**Same key twice in a dictionary**  
when: a dictionary literal repeats a key so the later value wins  
fields: {file}, {line}, {col}, {name} (key)

- Duplicate key {name} at line {line} in {file}
- Key {name} appears more than once
- {file} has repeated key {name}
- Later value wins for key {name}
- Double key {name} in the dictionary
- {name} key is duplicated on line {line}
- Repeated key {name}
- {name} used twice as key
- Duplicate dictionary key
- {file} repeats key {name}
- Key collision on {name}
- {name} appears twice
- Later {name} overwrites
- Duplicate in dict {name}
- {file} has two {name} keys
- Key {name} not unique
- Repeated {name} in map
- {file} line {line} duplicate key
- {name} key conflict
- Dict has two {name}
- Overwriting key {name}
- {name} defined twice in dict
- Duplicate key found
- {file} dict key clash
- Key {name} repeated

### warn.other
**Any other warning**  
when: a warning that has no situation of its own above  
fields: {file}, {line}, {col}

- Warning at line {line} in {file}
- Something to check in {file} around {line}:{col}
- {file} has a warning on line {line}
- Mild issue at {line}:{col} in {file}
- Warning spotted in {file} line {line}
- {file} triggered a general warning
- General warning in {file}
- Something odd at line {line}
- {file} has a soft issue
- Warning during check
- {file} line {line} warning
- Mild problem found
- Warning generated for {file}
- {file} has something to note
- Soft error at {line}:{col}
- Warning in the code
- {file} produced a warning
- Check this at line {line}
- {file} has a notice
- General warning raised
- Something worth a look
- {file} warning at {line}
- Mild concern in {file}
- Warning issued
- Note this in {file}

## A6  Limits and memory

### memory.warn.80
**Compiler memory at 80 percent**  
when: the compiler memory budget is at least 80 percent used  
fields: {percent}, {limit}

- Memory at {percent} percent of {limit}
- Getting close to the memory limit
- {percent} percent used already
- Memory pressure at {percent} percent
- Approaching the {limit} budget
- {percent} percent of memory gone
- High memory use {percent}
- Memory is filling up
- {percent} percent is high
- Nearing the limit
- Memory warning at {percent}
- Budget is {percent} used
- {limit} budget under pressure
- Memory climbing to {percent}
- High usage warning
- {percent} percent reached
- Memory getting tight
- Close to the edge
- {percent} of {limit} used
- Memory caution
- Usage high at {percent}
- Approaching full
- Memory load {percent}
- Budget warning
- {percent} percent mark hit

### memory.abort.95
**Compiler memory at 95 percent so it stopped**  
when: the compiler stopped cleanly because the budget is at least 95 percent used  
fields: {percent}, {limit}

- Stopped at {percent} percent of {limit}
- Memory budget almost gone {percent} percent
- Had to quit at {percent} percent usage
- {percent} percent is too close to the edge
- Memory limit nearly hit so I stopped
- Aborting at {percent} percent of {limit}
- Memory too high stopped
- {percent} percent forced stop
- Budget nearly exhausted
- Clean stop at {percent}
- Memory safety stop
- {percent} percent is the line
- Stopped to protect the system
- Memory abort at {percent}
- Too much memory used
- Halted at high usage
- {percent} percent trigger
- Memory limit action
- Stopped cleanly
- Budget protection engaged
- {percent} percent reached stop
- Memory full enough
- Abort for safety
- High memory stop
- {limit} nearly gone

### memory.waiting.shared
**Waiting for shared memory budget**  
when: other csyc processes are using the shared budget so this build waits  
fields: {time}

- Waiting for shared memory for {time}
- Other processes are using the budget
- Held up on shared memory for {time}
- Shared budget is busy right now
- Waiting my turn for memory
- Shared memory wait time {time}
- Delayed by shared memory
- Other builds are active
- Memory shared and busy
- Waiting on other processes
- Shared resource wait
- {time} spent waiting
- Budget in use by others
- Queued for memory
- Shared memory contention
- Waiting for free budget
- Other csyc holding memory
- Delay due to sharing
- Memory wait in progress
- {time} of wait so far
- Shared pool busy
- Holding for memory
- Other process priority
- Wait state for budget
- Shared memory lock

### memory.project.too.big
**Project needs more memory than the budget**  
when: the project cannot fit in the memory budget  
fields: {size}, {limit}

- Project needs {size} but limit is {limit}
- Too big for the memory budget
- {size} required versus {limit} allowed
- Project will not fit in memory
- Memory budget too small for this
- Needs more than {limit} to run
- Project oversized for budget
- {size} is beyond {limit}
- Cannot fit the project
- Memory demand too high
- Project exceeds budget
- {limit} is not enough
- Need larger memory limit
- Project too heavy
- Budget insufficient
- {size} over the allowed
- Cannot allocate enough
- Project memory demand high
- Limit too low for project
- {size} required more than {limit}
- Memory shortfall
- Project will not load
- Budget too tight
- Need more memory headroom
- Project size problem

### memory.system.out
**The system ran out of memory**  
when: the operating system refused memory  
fields: {size}

- System refused memory request of {size}
- Out of system memory at {size}
- OS said no to {size} more memory
- System memory is exhausted
- Cannot allocate {size} from the system
- Host ran out of memory
- System OOM for {size}
- No more system memory
- Allocation failed at system level
- OS memory denied
- System cannot provide {size}
- Host memory gone
- Out of memory from OS
- System level failure
- Cannot get {size} from host
- OS refused the request
- System memory empty
- Allocation rejected by system
- Host is out
- System RAM exhausted
- No system memory left
- OS out of resources
- Memory request denied
- System cannot help
- Host memory failure

### source.big
**A very large source file**  
when: a source file is large (you decide from what size this should speak, see the question at the end of this block)  
fields: {file}, {size}

- {file} is a big one at {size}
- Large source file {file} size {size}
- {file} came in heavy at {size}
- That is a hefty file {file}
- Big source {file} weighing {size}
- {file} is large {size}
- Substantial file {file}
- {file} is sizeable
- Large input {file}
- {file} has real weight
- Big file detected {file}
- {size} is notable for {file}
- {file} is on the large side
- Hefty source {file}
- {file} comes in large
- Sizeable {file}
- {file} is not small
- Large codebase piece {file}
- {file} has bulk
- Big one {file}
- {file} size stands out
- Notable size on {file}
- {file} is substantial
- Large file alert
- {file} is big

## A7  While the program runs

### runtime.division.zero
**Division by zero**  
when: the program divided by zero  
fields: {file}, {line}

- Division by zero at line {line} in {file}
- Zero division crash in {file}
- {file} divided by zero on line {line}
- Cannot divide by zero at line {line}
- Runtime zero division in {file}
- Math broke at line {line} of {file}
- Divide by zero in {file}
- Zero in the denominator
- {file} hit divide by zero
- Math error zero division
- Division by zero runtime
- {file} line {line} zero div
- Cannot compute that division
- Zero divisor found
- Runtime math error
- {file} divided by nothing
- Zero division exception
- Bad division at line {line}
- {file} has zero divide
- Math fault zero
- Division error in {file}
- Zero on the bottom
- Runtime divide fail
- {file} math broke
- Division by zero hit

### runtime.index.range
**Index out of range**  
when: an index is outside a vector, string, or array  
fields: {file}, {line}, {count} (the index)

- Index {count} out of range at line {line} in {file}
- Bad index {count} in {file}
- {file} accessed index {count} which is invalid
- Out of bounds index {count} on line {line}
- Index {count} does not exist there
- Range error with index {count} in {file}
- Index {count} is invalid
- Out of range access
- {file} bad index {count}
- Index beyond end
- {count} is too far
- Bounds error on index
- {file} line {line} bad index
- Index {count} out of bounds
- Invalid index {count}
- Range check failed
- {file} index error
- Too large index {count}
- Negative or high index
- Bounds violation
- Index {count} illegal
- {file} accessed bad slot
- Out of range {count}
- Index fault
- Bad access at {count}

### runtime.recursion.limit
**Recursion too deep**  
when: a function called itself past the recursion limit  
fields: {file}, {line}, {name} (function), {limit}

- Recursion limit {limit} hit by {name} in {file}
- {name} called itself too many times
- Stack too deep for {name} at line {line}
- {file} recursion on {name} past {limit}
- Too much recursion in {name}
- Recursion limit broken by {name}
- Deep recursion in {name}
- {name} stacked too far
- Recursion overflow
- {limit} calls deep on {name}
- Stack limit hit by {name}
- {file} recursive {name} too deep
- Too many self calls
- Recursion safety triggered
- {name} went too deep
- Stack depth exceeded
- {file} recursion problem
- Limit {limit} on {name}
- Recursive call flood
- {name} recursion maxed
- Deep call stack
- Recursion abort
- {name} called past limit
- Stack overflow style
- Too recursive {name}

### runtime.out.of.memory
**Program ran out of memory**  
when: the program asked for more memory than is available  
fields: {file}, {size}

- Program out of memory needing {size} in {file}
- {file} asked for too much memory
- Runtime memory fail at {size}
- Not enough memory for {file}
- Allocation of {size} failed in {file}
- Program memory exhausted
- Runtime OOM in {file}
- Memory request {size} failed
- Program cannot get memory
- Out of memory during run
- {file} memory hungry
- Allocation failed
- Runtime memory error
- {size} not available
- Program ran dry on memory
- Memory allocation fail
- {file} exhausted memory
- No memory left for program
- Runtime out of RAM
- Memory denied to program
- {file} needs more than available
- Program memory crisis
- Allocation of {size} refused
- Runtime memory short
- Out of memory crash

### runtime.integer.overflow
**Integer overflow**  
when: an integer calculation overflowed  
fields: {file}, {line}

- Integer overflow at line {line} in {file}
- Number got too big in {file}
- Overflow on line {line} of {file}
- Integer math overflowed in {file}
- Value overflowed at line {line}
- Overflow error in {file} line {line}
- Integer too large
- Math overflow in {file}
- Number wrapped or failed
- Overflow at runtime
- {file} integer overflow
- Calculation too big
- Integer limit hit
- Overflow on that math
- {file} line {line} overflow
- Number out of range
- Integer calculation failed
- Overflow detected
- Math result too large
- {file} has overflow
- Runtime integer problem
- Value too big
- Overflow in computation
- Integer fault
- Number overflowed

### runtime.type
**Wrong kind of value**  
when: for example indexing a value that cannot be indexed, or a dictionary key that is not a string  
fields: {file}, {line}

- Type error at line {line} in {file}
- Wrong kind of value in {file}
- {file} has a type mismatch on line {line}
- Cannot do that with this type at line {line}
- Type problem in {file} line {line}
- Value type is wrong at {line}
- Type mismatch runtime
- Wrong type used
- {file} type error
- Incompatible type
- Type check failed at runtime
- {file} line {line} type issue
- Bad type for operation
- Type error in value
- Wrong type at runtime
- {file} has type fault
- Operation on wrong type
- Type mismatch found
- Runtime type problem
- Value is wrong kind
- Type error hit
- {file} type wrong
- Incompatible value
- Type fault at line {line}
- Wrong type for that

### runtime.not.callable
**Called something that is not a function**  
when: a value that is not a function was called  
fields: {file}, {line}, {name}

- {name} is not callable at line {line} in {file}
- Tried to call non function {name}
- {file} called {name} which is not a function
- Not a function: {name} on line {line}
- Cannot call {name} like that
- {name} is not callable in {file}
- {name} cannot be called
- Non callable {name}
- {file} tried to call {name}
- {name} is not a function
- Call on non function
- {name} has no call
- Attempt to call {name}
- {file} line {line} bad call
- {name} is data not code
- Cannot invoke {name}
- Non function call
- {name} not invokable
- Call target wrong
- {file} called wrong thing
- {name} is not callable
- Invalid call target
- Tried calling {name}
- Not a callable value
- Call failed on {name}

### runtime.unknown.builtin
**Unknown built in**  
when: a call names a built in function that does not exist  
fields: {file}, {line}, {name}

- Unknown built in {name} at line {line} in {file}
- No such built in {name}
- {file} called missing built in {name}
- Built in {name} does not exist
- Unknown function {name} on line {line}
- No built in named {name} in {file}
- {name} is not a built in
- Missing built in {name}
- Unknown {name} function
- No such built in
- {file} has unknown built in
- {name} not recognized
- Built in lookup failed
- {name} is not built in
- Unknown name {name}
- No built in {name}
- {file} line {line} unknown built in
- {name} does not exist as built in
- Invalid built in call
- {name} not found in builtins
- Built in missing
- Unknown {name}
- No function {name}
- Built in error
- {name} is invalid built in

### runtime.other
**Any other runtime error**  
when: a runtime error that has no situation of its own above  
fields: {file}, {line}

- Runtime error at line {line} in {file}
- Something crashed in {file} at line {line}
- {file} hit a runtime problem on line {line}
- Unexpected runtime fail at {line}
- Runtime issue in {file} line {line}
- Program errored at line {line} of {file}
- Runtime fault in {file}
- Crash at line {line}
- {file} runtime problem
- Unexpected error during run
- Runtime failure
- {file} has runtime error
- Problem at runtime line {line}
- {file} crashed
- Runtime exception
- Error during execution
- {file} line {line} runtime fail
- Unexpected runtime issue
- Program fault at line {line}
- Runtime problem found
- {file} had a runtime error
- Execution error
- Runtime crash
- Fault in running code
- Unexpected fail at runtime

## A8  Speed notes

### native.active
**Native fast path is active**  
when: some functions run as native code  
fields: {count} (functions)

- Native path active for {count} functions
- {count} functions running native
- Fast path on for {count} functions
- Native code engaged for {count}
- {count} functions got the native boost
- Native speed for {count} functions
- {count} in native mode
- Native acceleration active
- Fast path for {count}
- {count} functions native
- Native enabled on {count}
- Speed boost for {count}
- {count} running native
- Native path live
- {count} functions accelerated
- Native mode on
- Fast native for {count}
- {count} in fast path
- Native active
- Acceleration for {count}
- {count} native functions
- Native path working
- Speed path for {count}
- {count} got native
- Native engaged

### native.partial
**Only some functions went native**  
when: part of the functions stayed interpreted  
fields: {count} (native functions), {size} (total functions)

- Only {count} of {size} functions went native
- Partial native: {count} out of {size}
- Some functions native {count} total {size}
- Mixed mode {count} native of {size}
- Not everything went native {count} did
- {count} native {size} total
- Partial acceleration
- {count} of {size} native
- Mixed native and interpreted
- Some native some not
- {count} accelerated out of {size}
- Partial fast path
- {size} total {count} native
- Incomplete native coverage
- {count} got native rest did not
- Mixed execution mode
- Partial native success
- {count} native remaining interpreted
- Some functions fast
- Native partial on {count}
- {size} functions {count} native
- Incomplete boost
- Mixed path used
- {count} out of {size} fast
- Partial native mode

### native.none
**Nothing went native**  
when: no function could use the native fast path  
fields: {file}

- No native path for {file}
- Everything stayed interpreted in {file}
- Native fast path not available
- {file} running fully interpreted
- No functions went native
- Pure interpreted mode for {file}
- Native unavailable
- All interpreted in {file}
- No fast path
- {file} fully slow path
- Native not engaged
- Interpreted only
- No native functions
- {file} without native
- Fast path skipped
- All code interpreted
- Native path dark
- {file} no acceleration
- Pure interpreter mode
- No native boost
- {file} stayed interpreted
- Native none
- Fast path off
- Everything interpreted
- No native for {file}

## A9  Command line

### cli.unknown.command
**Unknown command or option**  
when: csyc was given a command or option it does not know  
fields: {name}

- Unknown command or option {name}
- Do not know what {name} is
- {name} is not a valid option
- Unrecognized {name}
- No such command {name}
- {name} is not something I understand
- Unknown {name}
- {name} not recognized
- Invalid option {name}
- No command named {name}
- {name} is unknown
- Unrecognized flag or command
- {name} does not exist
- Bad command {name}
- Unknown input {name}
- {name} not supported
- Invalid {name}
- No such option
- {name} is not valid
- Unknown argument {name}
- Command not found {name}
- {name} unknown
- Not a known option
- {name} invalid
- Unrecognized {name}

### cli.missing.file
**No file given**  
when: a command needs a file and none was given  
fields: {name} (command)

- {name} needs a file and none was given
- Missing file for command {name}
- No file provided to {name}
- {name} requires a file argument
- File missing for {name}
- Give {name} a file next time
- No file for {name}
- {name} expects a file
- File argument missing
- {name} without file
- Need a file for {name}
- Missing input file
- {name} file required
- No source given to {name}
- File not supplied
- {name} needs input
- Absent file argument
- {name} incomplete without file
- Provide a file for {name}
- File required by {name}
- No file argument
- {name} missing its file
- Input file absent
- {name} has no file
- File needed

### cli.bad.option.value
**Bad value for an option**  
when: for example --memory-limit without a number  
fields: {name} (option)

- Bad value for option {name}
- {name} got an invalid value
- Option {name} value is wrong
- Cannot parse value for {name}
- {name} expects a proper value
- Invalid argument to {name}
- Bad {name} value
- {name} value incorrect
- Invalid setting for {name}
- {name} has bad argument
- Value error on {name}
- Cannot use that with {name}
- {name} value not accepted
- Bad parameter for {name}
- {name} expects better value
- Invalid {name} input
- Value for {name} is off
- {name} argument bad
- Cannot process {name} value
- Wrong value type for {name}
- {name} value problem
- Bad input to {name}
- {name} rejected the value
- Invalid option value
- {name} value fails

## A10  Output of the Cooler Compiler commands themselves

### ccm.enabled
**After csyc enable ccm**  
when: the Cooler Compiler was switched on  
fields: none

- Cooler Compiler is now on
- Alright it is enabled
- CCM switched on
- You turned me on
- Cooler mode active
- Enabled and ready
- Now online
- Cooler is live
- Activated
- On and listening
- Enabled successfully
- Cooler Compiler active
- You flipped it on
- Now in cooler mode
- Ready and enabled
- Switched on
- CCM is up
- Cooler online
- Enabled for you
- Now active
- Cooler mode engaged
- On as requested
- Activated and here
- Cooler is enabled
- Live now

### ccm.already.enabled
**Enable when already on**  
when: csyc enable ccm was run while it was already on  
fields: none

- Already on you know
- It was already enabled
- No need to enable twice
- Still on same as before
- Already running in cooler mode
- You already turned it on
- Already active
- It is already enabled
- No change needed
- Still on
- Already in cooler mode
- Redundant enable
- Already live
- Was already on
- No action taken
- Already enabled
- Still active
- Already there
- On already
- No need
- Already running
- Enabled previously
- Still cooler
- Already set
- On as before

### ccm.disabled
**After csyc disable ccm**  
when: the Cooler Compiler was switched off (see the question at the end about whether this line is still spoken)  
fields: none

- Cooler Compiler is now off
- Alright switched off
- CCM disabled
- Back to quiet mode
- Disabled as requested
- Off for now
- Now offline
- Cooler is off
- Deactivated
- Quiet mode on
- Disabled successfully
- Cooler Compiler inactive
- You flipped it off
- Back to normal
- Off and quiet
- Switched off
- CCM is down
- Cooler offline
- Disabled for you
- Now inactive
- Cooler mode disengaged
- Off as requested
- Deactivated and gone
- Cooler is disabled
- Quiet now

### ccm.already.disabled
**Disable when already off**  
when: csyc disable ccm was run while it was already off  
fields: none

- Already off
- It was already disabled
- No need to disable again
- Still off
- Already in quiet mode
- You already turned it off
- Already inactive
- It is already disabled
- No change needed
- Still quiet
- Already off
- Redundant disable
- Already quiet
- Was already off
- No action taken
- Already disabled
- Still inactive
- Already there
- Off already
- No need
- Already quiet
- Disabled previously
- Still normal
- Already set
- Off as before

### ccm.pruned
**After csyc prune**  
when: the memory was pruned  
fields: {count} (records kept)

- Pruned down to {count} records
- Memory cleaned kept {count}
- Prune done {count} left
- Trimmed to {count} records
- Kept the last {count}
- Prune finished with {count} remaining
- Now at {count} records
- Cleaned to {count}
- {count} records remain
- Pruned successfully
- Memory now {count}
- Kept {count}
- Trim complete
- {count} left after prune
- Records reduced to {count}
- Prune to {count} done
- Memory trimmed
- {count} records kept
- Cleanup finished
- Now holding {count}
- Pruned to size
- {count} surviving records
- Memory reduced
- Kept recent {count}
- Prune success

### ccm.wiped
**After csyc wipe**  
when: the memory was wiped  
fields: none

- Memory wiped clean
- Everything gone
- Full wipe done
- Memory is empty now
- All records deleted
- Wiped as requested
- Clean slate
- Memory cleared
- Nothing left
- Full reset done
- All gone
- Wipe complete
- Memory empty
- Records erased
- Clean memory
- Everything removed
- Wipe successful
- Blank memory
- All cleared
- Reset finished
- Memory zeroed
- Nothing remains
- Full wipe
- Cleaned out
- Empty now

### ccm.cancelled
**Answered n to a question**  
when: you answered no to the are you sure question  
fields: none

- Cancelled nothing changed
- Ok keeping everything
- You said no so stopped
- Action cancelled
- Nothing was deleted
- Backing out as requested
- Cancelled by you
- No action taken
- Kept as is
- Stopped the operation
- You declined
- Nothing modified
- Cancel confirmed
- Aborted the change
- Left everything alone
- No wipe or prune
- Cancelled safely
- You chose no
- Operation stopped
- No changes made
- Backed out
- Declined and safe
- Nothing touched
- Cancel done
- Safe cancel

### ccm.nothing.to.prune
**Nothing to prune**  
when: the memory has no more records than you asked to keep  
fields: {count}

- Nothing to prune already at {count}
- Already down to {count} or less
- No extra records to remove
- Memory is already lean
- Nothing extra beyond {count}
- Prune not needed
- Already at target
- No pruning required
- Memory already small
- {count} is already the size
- Nothing excess
- Already trimmed
- No work to do
- Memory at or under {count}
- Prune skipped
- Already good
- No records to drop
- Target already met
- Nothing surplus
- Already clean
- No prune action
- Memory fine as is
- Already at {count}
- Nothing to cut
- Skip prune

### ccm.memory.empty
**Memory already empty**  
when: wipe or prune was run and the memory has nothing in it  
fields: none

- Memory is already empty
- Nothing in there to touch
- Already wiped clean
- Empty memory nothing to do
- No records present
- Memory was already clear
- Already empty
- Nothing to wipe
- Memory blank
- No content
- Already clear
- Empty as can be
- Nothing stored
- Memory vacant
- No records exist
- Already gone
- Blank already
- Nothing present
- Empty state
- No data in memory
- Already wiped
- Vacant memory
- Nothing there
- Clear already
- Empty memory

## A11  Your own situations (Part A)

### own.a.1
when, in plain words (what happens that makes the compiler speak?): first successful compile after enabling cooler compiler
fields you want: {file}

- First clean compile with me around {file}
- Look at that {file} works and I am watching
- Nice start {file} first success under cooler mode
- {file} compiled and I am here for it
- Welcome to the cooler side {file}
- First one in the books {file}
- Debut success on {file}
- First win with cooler on
- {file} opens the cooler era
- Initial clean compile
- First success recorded
- {file} starts the memory
- Cooler mode first win
- {file} behaved first try
- Opening success
- First clean one
- {file} under watch and good
- Debut of cooler success
- First recorded win
- {file} first success
- Cooler debut clean
- Starting strong with {file}
- First one done
- {file} begins well
- Initial success logged

### own.a.2
when, in plain words (what happens that makes the compiler speak?): compile took longer than usual
fields you want: {file}, {time}

- That took a while {file} {time}
- {file} was slow this time {time}
- Longer than usual on {file}
- {time} for {file} felt a bit heavy
- {file} made me wait {time}
- Slow one on {file} at {time}
- Extended compile on {file}
- {file} took its time
- Longer compile than expected
- {time} is notable for {file}
- {file} was not quick
- Slow compile detected
- {file} lingered at {time}
- Took longer than average
- {file} compile was heavy
- Extended time on {file}
- {time} felt long
- {file} slow path
- Compile duration high
- {file} took extra time
- Noticeable duration
- {file} was deliberate
- Slow run for {file}
- {time} on the clock
- Longer than typical

### own.a.3
when, in plain words (what happens that makes the compiler speak?): 
fields you want: 

- 
- 
- 
- 
- 
- 

### own.a.4
when, in plain words (what happens that makes the compiler speak?): 
fields you want: 

- 
- 
- 
- 
- 
- 

### own.a.5
when, in plain words (what happens that makes the compiler speak?): 
fields you want: 

- 
- 
- 
- 
- 
- 

---

# Part B: what the compiler remembers

These situations only exist because the compiler keeps a memory in `ccm_mem.bin`. The memory is a list of records of your compile attempts. The words are still yours, from this file.

## B1  What may it remember? (tick what you allow)

Each record is one compile attempt. Tick the facts it may keep:

- [x] the time of every attempt
- [x] the path of the file
- [x] a fingerprint (hash) of the file contents, never the contents: this is what tells it that it is the exact same file even if the file was renamed or moved
- [x] success or failure
- [x] which situation from Part A came first when it failed
- [x] how many errors and how many warnings
- [x] whether it was a build or a run

Anything you leave unticked, the compiler will not store, and every situation below that needs it will be dropped. I will tell you which ones.

## B2  What does this exact file mean?

- [ ] the same path
- [x] the same contents, even if renamed or moved (needs the fingerprint)
- [ ] the same path AND the same contents
- [ ] other: 

## B3  Memory situations

### mem.first.ever
**Very first thing it remembers**  
when: the memory is empty, this is the first compile it has seen  
fields: {file}

- First time ever seeing a file and it is {file}
- Memory starts here with {file}
- Brand new memory and {file} is the first
- Hello {file} you are the first one
- Empty memory until {file} showed up
- First record ever is {file}
- Debut file {file}
- Memory begins with {file}
- First ever compile {file}
- {file} opens the book
- Initial memory entry
- {file} is number one
- Starting the log with {file}
- First sighting {file}
- Memory birth is {file}
- {file} the original
- First in history
- {file} starts everything
- Empty until {file}
- First contact {file}
- Memory online with {file}
- {file} is the pioneer
- Opening entry {file}
- First one logged
- {file} begins the memory

### mem.new.file
**A file it has never seen**  
when: this file is not in the memory yet (the memory has other files)  
fields: {file}

- New file {file} never seen this one
- Fresh face {file}
- {file} is new around here
- Have not met {file} before
- Adding {file} to the memory
- Unknown file until now {file}
- First meeting {file}
- New entry {file}
- {file} is unfamiliar
- Introducing {file}
- Fresh file detected
- {file} joins the memory
- Never seen {file}
- New to me {file}
- {file} first appearance
- Unknown until {file}
- Adding new {file}
- {file} is novel
- First time for {file}
- New file in the log
- {file} not in history
- Fresh addition {file}
- {file} is a newcomer
- Meeting {file}
- New record for {file}

### mem.same.file.failed.once
**This exact file failed once before**  
when: this exact file failed exactly one time before and is being compiled again  
fields: {file}, {time}, {error}

- You failed this exact file once already
- Same file failed before now trying again
- {file} failed once last time
- One prior failure on this exact file
- Back again after one fail on {file}
- Second chance for this exact file
- One previous fail on this
- {file} has one prior fail
- Failed once before
- Trying again after one fail
- One strike on this file
- {file} second attempt after fail
- Previously failed once
- One fail in history
- {file} failed earlier once
- Single prior failure
- Back after one miss
- One previous error
- {file} has a past fail
- Retry after one fail
- Failed this once already
- One fail on record
- Second try after fail
- {file} once failed
- One earlier failure

### mem.same.file.failed.many
**This exact file failed several times before**  
when: this exact file failed two or more times before  
fields: {file}, {count}, {time}, {error}

- This exact file failed {count} times already
- {count} prior fails on this same file
- Still failing this file after {count} tries
- {file} has failed {count} times before
- Multiple fails on this exact one
- {count} strikes on this same file so far
- {count} previous fails
- Repeated fails on this
- {file} keep failing
- Many prior fails {count}
- {count} fails in the past
- Still not working after {count}
- Multiple previous errors
- {file} has history of fails
- {count} times already
- Persistent fail on this
- {count} earlier fails
- This file keeps failing
- {count} fails recorded
- Long fail history
- {file} failed often
- Many attempts failed
- {count} prior problems
- Repeated failure pattern
- {count} fails so far

### mem.same.file.now.succeeded
**This exact file finally compiled**  
when: this exact file compiled now after failing before  
fields: {file}, {count} (failures before), {time}

- Finally compiled after {count} fails
- This exact file made it after {count} tries
- {file} succeeded after {count} failures
- Took {count} fails but it works now
- Breakthrough after {count} previous fails
- {file} is good after {count} bad runs
- Success after {count} fails
- Finally after {count}
- {file} broke the fail streak
- Won after {count} losses
- {count} fails then success
- Made it after {count}
- {file} finally clean
- Success following fails
- After {count} now good
- Breakthrough on {file}
- {count} then success
- Finally working
- {file} overcame the fails
- Win after {count}
- Succeeded at last
- {count} fails behind
- Now succeeding
- {file} turned around
- Success after struggle

### mem.same.error.again
**Same kind of error as last time**  
when: the first error is the same situation as the first error last time on this file  
fields: {file}, {error}, {count}

- Same error {error} again on {file}
- {error} keeps showing up
- You hit {error} once more
- Same problem {error} for the {count} time
- {error} is back on this file
- Repeated {error} on {file}
- {error} again
- Same issue returns
- {error} repeats
- Familiar error {error}
- {error} once more
- Same as last time
- {error} persists
- Repeated problem
- {error} again on {file}
- Same error type
- {error} keeps appearing
- Familiar fail
- {error} is persistent
- Same again
- {error} returns
- Repeat of {error}
- {error} still here
- Same error pattern
- {error} continues

### mem.different.error
**A different error than last time**  
when: this file failed again but the first error is a different situation  
fields: {file}, {error}

- Different error this time {error}
- New problem {error} on {file}
- Not the same error as last time
- Fresh error {error} appeared
- {file} failed with a new {error}
- Different kind of fail {error}
- New error {error}
- Changed problem
- Different issue now
- {error} is new
- Fresh fail type
- Not the previous error
- New kind of problem
- {error} this time
- Different from before
- New error surfaced
- {file} has new {error}
- Changed the error
- Different fail
- {error} instead
- New problem type
- Error changed
- Different this run
- Fresh issue {error}
- Not the same

### mem.unchanged.failed
**Nothing changed and it failed again**  
when: the contents of the file are identical to the last failed attempt  
fields: {file}, {count}, {time}

- Nothing changed and it still fails
- Same exact contents still broken
- You did not change anything and it failed
- Identical file still failing after {count}
- No edits and still the same fail
- Unchanged and still not working
- No changes still fail
- Same file same fail
- Unedited and broken
- Nothing different still error
- Identical and failing
- No modification still bad
- Same content same result
- Unchanged fail continues
- No edit still fail
- Same as last fail
- Nothing altered
- Still failing unchanged
- Identical contents fail
- No change no progress
- Same file still bad
- Unmodified and error
- Still the same problem
- No difference still fail
- Unchanged failure

### mem.changed.still.fails
**Changed but still failing**  
when: the contents changed since the last failure and it still fails  
fields: {file}, {count}

- You changed it but it still fails
- Edits happened and still broken
- Changed file still failing after {count}
- New contents same result fail
- {file} was edited and still errors
- Changes were not enough
- Edited but still fail
- Changes did not fix it
- Still failing after edits
- Modified and still bad
- Changes insufficient
- Still broken after change
- Edited yet failing
- New version still fails
- Changes missed the mark
- Still error after edit
- Modified but no success
- Changes not enough
- Still failing post edit
- Edit did not solve
- Changed and still down
- Still errors after change
- Modifications failed
- Still not working
- Edit incomplete

### mem.streak.fail
**Failures in a row**  
when: several compile attempts in a row failed (any files)  
fields: {count}

- {count} fails in a row
- Failure streak at {count}
- {count} bad compiles back to back
- Streak of fails continues {count}
- Another fail makes it {count}
- {count} failures straight
- Fail streak {count}
- {count} in a row bad
- Consecutive fails {count}
- Streak growing {count}
- {count} fails sequential
- Running fail count {count}
- Back to back fails
- {count} straight failures
- Fail run at {count}
- Consecutive errors
- {count} fails consecutive
- Streak of {count}
- Failures mounting
- {count} in succession
- Ongoing fail streak
- {count} bad ones
- Sequential fails
- Fail chain {count}
- Streak continues

### mem.streak.success
**Successes in a row**  
when: several compile attempts in a row succeeded (any files)  
fields: {count}

- {count} successes in a row
- Winning streak at {count}
- {count} clean compiles back to back
- Success streak continues {count}
- Another win makes it {count}
- {count} good ones straight
- Success streak {count}
- {count} in a row good
- Consecutive wins {count}
- Streak growing {count}
- {count} successes sequential
- Running win count {count}
- Back to back successes
- {count} straight wins
- Success run at {count}
- Consecutive cleans
- {count} successes consecutive
- Streak of {count}
- Wins mounting
- {count} in succession
- Ongoing success streak
- {count} good ones
- Sequential successes
- Win chain {count}
- Streak continues

### mem.comeback
**First success after a streak of failures**  
when: a success right after a run of failures  
fields: {count} (failures in the streak)

- Comeback after {count} fails
- Finally a win after {count} losses
- Broke the fail streak of {count}
- Success after {count} bad ones
- Streak snapped after {count} fails
- Back on track after {count}
- Win after {count} fails
- Streak broken
- Success ends the run
- After {count} now good
- Comeback success
- Fail streak over
- {count} then success
- Recovered after {count}
- Win ends the fails
- Streak of fails ends
- Success returns
- After the drought
- {count} fails then win
- Comeback complete
- Fail run stopped
- Success after struggle
- Streak terminated
- Win after losses
- Recovered

### mem.many.successes.same.file
**This file keeps compiling fine**  
when: this exact file compiled successfully many times before  
fields: {file}, {count}

- This file has succeeded {count} times
- {file} keeps working {count} successes
- Reliable file {file} with {count} wins
- {count} clean compiles on this exact file
- {file} is consistent at {count} successes
- Many wins on {file} already {count}
- {count} successes on this
- Reliable {file}
- {file} keeps succeeding
- Consistent winner
- {count} good runs
- {file} is steady
- Many clean compiles
- {file} trusted
- {count} wins recorded
- Steady success
- {file} performs
- Consistent {file}
- Many successes
- {file} is solid
- {count} times good
- Reliable performer
- {file} keeps delivering
- Steady wins
- {count} and counting

### mem.been.a.while
**Long time since the last compile**  
when: a long time passed since the last attempt in the memory (you decide how long, see the question at the end)  
fields: {time}

- Been a while {time} since last time
- Long gap of {time} since the last compile
- {time} has passed since I saw you
- Welcome back after {time}
- It has been {time} since the last one
- Long time no see {time}
- {time} since last
- Long absence
- Gap of {time}
- Welcome back
- Long time away
- {time} gap
- Been gone a while
- Missed you for {time}
- Long interval
- {time} later
- Back after long
- Extended gap
- {time} passed
- Long wait over
- Absence of {time}
- Returned after {time}
- Long time between
- Gap closed
- {time} since

## B4  Your own memory situations

### own.b.1
when, in plain words (what must the compiler remember and notice?): user fixed a specific error that appeared multiple times
fields you want: {file}, {error}, {count}

- You finally fixed {error} after {count} times
- {error} is gone after {count} repeats on {file}
- Nice {error} stayed dead this time
- Fixed the repeating {error} on {file}
- {count} times was enough for {error}
- That stubborn {error} is gone
- {error} defeated after {count}
- Fixed the recurring one
- {error} no longer appears
- After {count} now fixed
- {error} resolved
- Repeating error fixed
- {file} no longer has {error}
- {error} cleared
- Finally rid of {error}
- {count} then fixed
- Persistent error gone
- {error} solved
- No more {error}
- Fixed at last
- {error} eliminated
- After many tries fixed
- {error} is history
- Cleared the repeat
- {error} no longer

### own.b.2
when, in plain words (what must the compiler remember and notice?): 

- 
- 
- 
- 
- 
- 

### own.b.3
when, in plain words (what must the compiler remember and notice?): 

- 
- 
- 
- 
- 
- 

and so on...