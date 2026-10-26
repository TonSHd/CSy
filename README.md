# CSy

> "CSy is like python and C++ split custody"

CSy is an AI focused compiled language built to make AI development less
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

`csyc build` reads `project.csp` in the current directory. Pass a different
`.csp` path to select another project. If its `output` is omitted, the build
writes a `.cso` beside the entry source. A source build without `-o` also writes
a same-basename `.cso`. The `-o` option is currently limited to one `.csy`
file. `csyc run name.cso` rebuilds when a matching `name.csy` is newer; when the
artifact matches the current directory's `project.csp` it checks the loaded
project sources as well. Otherwise it runs the existing object.

Not gonna brag but you can alias almost everything in CSy using "as, using"

You can learn everything in our learning document in CSY_SYNTAX_REFERENCE.md!
CSy was still in early version (1.0.0) so you might encounter weird stuff.
But don't just ignore it! Tell us in issues

## Warnings

Treat this language as a joke language.
This language was terrible at some stuff (and i mean it)

it was mostly for Machine Learning

## AI Distrubutions

ChatGPT, Claude, Z.ai(GLM), Deepseek

Right now.. CSy is Linux only.

We wanted to expand it to other OS but we don't have the hardware to test sadly.
Please stay tuned for updates.