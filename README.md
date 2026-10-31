# CSy

> "CSy is like python and C++ split custody"

CSy is an open source AI focused compiled language built to make AI development less
painful: concise, readable syntax, direct control over model architecture
and performance aimed at matching C++ on suitable workloads. The long term
goal is to make common AI work POTENTIALLY easier than Python without hiding the
architecture or forcing users into a framework.

CSy was built with C++

## Build

```bash
make
```

or

```bash
cmake -S . -B build && cmake --build build -j8
```

Then you can run

```bash
./csyc install
```

## BUT WAIT!

if you want "The Cooler Compiler" too, `csyc install` needs the file `ccm_table.bin` next to the `csyc` you built. It is in the repository root, so a `make` build already has it. If you built with CMake, `csyc` is in `build/`, so run `cp ccm_table.bin build/` first.

`csyc install` puts csyc in `/usr/bin` and the Cooler Compiler table in `/usr/share/csyc`.
CMake and the tarball's `install.sh` use `/usr` too. If an older `csyc` sits earlier on
your PATH (for example in `/usr/local/bin`), `csyc install` and `csyc doctor` tell you which
one runs and the exact `rm` command to remove it. Afterwards run `hash -r` or open a new
terminal.

## Quick start

```bash
csyc init myapp
cd myapp
csyc build
csyc run src/main.cso
```

`csyc init` makes `project.csp`, `src/main.csy` and a `.gitignore`. A single file
also works without a project: `csyc hello.csy`.

Statements end with `;`. A missing one is only a warning, but write it.

```
#inc basic as bc;

using bc::out as say;

say>"hello";
```

`csyc build` reads `project.csp` in the current directory. Pass a different
`.csp` path to select another project. If its `output` is omitted, the build
writes a `.cso` beside the entry source. A source build without `-o` also writes
a same-basename `.cso`. The `-o` option is currently limited to one `.csy`
file. `csyc run name.cso` rebuilds when a matching `name.csy` is newer; when the
artifact matches the current directory's `project.csp` it checks the loaded
project sources as well. Otherwise it runs the existing object.

What makes us special is...
We got a feature that can solves coding problem
Such as explaining how the code works on a specific part using "csyc explain"
There's more but i won't bloat README.md just to explain how features work. It's not the right place.

Not gonna brag but you can alias almost everything in CSy using "as, using"
You can customize how you code.

a simple example:

#inc basic as bc;

using bc::out as o;

o>"hello";

yes. its that simple.

you can do more than this:

#inc basic as bc;

using #inc as import;
import basic as me;

using me::out as say;

say>"hello";

Other things CSy has that are easy to miss:

- classes with inheritance: `class Dog(name) extends Animal(name):`
- error handling with `try` and `catch e:`
- several files in one program with `#connect "lib/math.csy";`
- loading and saving AI datasets: JSON, JSONL, Parquet, CSV, `.bin` tensors and text files (`bc::load_dataset`, `bc::save_dataset`)
- the Cooler Compiler, which stays off until you run `csyc enable ccm`

One thing that trips people: printing a comparison needs parentheses.
`o>(x == 5);` prints the result, `o>x == 5;` prints nothing.

You can learn everything including features in our learning document in CSY_SYNTAX_REFERENCE.md!
CSy was still in early version (1.1.0) so you might encounter weird stuff.
But don't just ignore it! Tell us in issues

## Warnings

Treat this language as a joke language.
This language was terrible at some stuff (and i mean it)

it was mostly for Machine Learning

## AI Assisted

ChatGPT, Claude, Z.ai(GLM), Deepseek, Grok

The AI tools are carefully managed, and their outputs are reviewed and monitored.

The final decisions about what goes into CSy remain under human direction.

Right now.. CSy is Linux only.

We wanted to expand it to other OS but we don't have the hardware to test sadly.
Please stay tuned for updates.
