CSY_DIR := src
VERSION := $(strip $(shell cat VERSION))
PREFIX ?= /usr
DESTDIR ?=
CXX := g++
NVCC ?= nvcc
CXXFLAGS := -std=c++17 -O3 -Wall -Wno-unused-parameter -Wno-unused-variable -MMD -MP -pthread
CXXFLAGS += -DCSYC_VERSION='"$(VERSION)"'
LDFLAGS := -ldl -pthread -static-libstdc++ -static-libgcc

ifneq ($(shell pkg-config --exists gmpxx 2>/dev/null && echo yes),)
CXXFLAGS += -DCSY_WITH_GMP=1 $(shell pkg-config --cflags gmpxx)
LDFLAGS += $(shell pkg-config --libs gmpxx)
endif

SOURCES := \
	$(CSY_DIR)/resource.cpp \
	$(CSY_DIR)/ccm.cpp \
	$(CSY_DIR)/osinfo.cpp \
	$(CSY_DIR)/lexer.cpp \
	$(CSY_DIR)/ast_printer.cpp \
	$(CSY_DIR)/parser.cpp \
	$(CSY_DIR)/project.cpp \
	$(CSY_DIR)/object_file.cpp \
	$(CSY_DIR)/linker.cpp \
	$(CSY_DIR)/explain.cpp \
	$(CSY_DIR)/diagnostic.cpp \
	$(CSY_DIR)/suggest.cpp \
	$(CSY_DIR)/semantic.cpp \
	$(CSY_DIR)/codegen.cpp \
	$(CSY_DIR)/ir.cpp \
	$(CSY_DIR)/value.cpp \
	$(CSY_DIR)/builtins_ext.cpp \
	$(CSY_DIR)/compress.cpp \
	$(CSY_DIR)/zstd.cpp \
	$(CSY_DIR)/json.cpp \
	$(CSY_DIR)/parquet.cpp \
	$(CSY_DIR)/csv.cpp \
	$(CSY_DIR)/dataset.cpp \
	$(CSY_DIR)/ai_ops.cpp \
	$(CSY_DIR)/pi.cpp \
	$(CSY_DIR)/gpu_nvidia.cpp \
	$(CSY_DIR)/native_jit.cpp \
	$(CSY_DIR)/tui.cpp \
	$(CSY_DIR)/vm.cpp \
	$(CSY_DIR)/main.cpp

OBJECTS := $(SOURCES:.cpp=.o)

csyc: $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I$(CSY_DIR) -c $< -o $@

$(CSY_DIR)/main.o: VERSION

-include $(OBJECTS:.o=.d)

clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) csyc

BINDIR := $(DESTDIR)$(PREFIX)/bin
MANDIR := $(DESTDIR)$(PREFIX)/share/man/man1
DOCDIR := $(DESTDIR)$(PREFIX)/share/doc/csyc
DATADIR := $(DESTDIR)$(PREFIX)/share/csyc

install: csyc
	install -d $(BINDIR) $(MANDIR) $(DATADIR) $(DOCDIR)/ccm
	install -s -m 0755 csyc $(BINDIR)/csyc
	gzip -9n < packaging/csyc.1 > $(MANDIR)/csyc.1.gz && chmod 0644 $(MANDIR)/csyc.1.gz
	install -m 0644 README.md $(DOCDIR)/
	install -m 0755 scripts/ccm_tools.py $(DOCDIR)/ccm/
	install -m 0644 ccm_table.bin $(DATADIR)/ccm_table.bin

uninstall:
	rm -f $(BINDIR)/csyc $(MANDIR)/csyc.1.gz
	rm -rf $(DOCDIR) $(DATADIR)

deb: csyc
	sh scripts/package.sh deb

tarball: csyc
	sh scripts/package.sh tar

.PHONY: clean cuda install uninstall deb tarball

cuda: csyc src/ai_cuda_kernels.cu
	@mkdir -p third_party/csy_ai_cuda
	$(NVCC) -O3 -std=c++17 -Xcompiler=-fPIC -shared src/ai_cuda_kernels.cu -lcublas -o third_party/csy_ai_cuda/libcsy_ai_cuda.so
