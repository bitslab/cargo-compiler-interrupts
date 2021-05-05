# BUILD_PATH?=/mnt/nilanjana
# LLVM_CONFIG?=$(BUILD_PATH)/bin/llvm-config
LLVM_CONFIG=llvm-config

ifndef VERBOSE
	QUIET:=@
endif

SRC_DIR?=$(PWD)
COMMON_FLAGS=-g -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -fPIC
CXXFLAGS+=$(COMMON_FLAGS) $(shell $(LLVM_CONFIG) --cxxflags)
LDFLAGS+=$(shell $(LLVM_CONFIG) --ldflags)
CPPFLAGS+=$(shell $(LLVM_CONFIG) --cppflags) -I$(SRC_DIR)

ifeq ($(shell uname), Darwin)
LOADABLE_MODULE_OPTIONS=-bundle -O3 -undefined dynamic_lookup
else
LOADABLE_MODULE_OPTIONS=-shared -O3
endif

LCPASS=libcompilerinterrupt.so
LCPASS_OBJECTS=compiler_interrupt.o

default: $(LCPASS)

$(LCPASS): $(SRC_DIR)/compiler_interrupt.cpp
	@echo "Compiling $*.cpp" 
	$(CXX) -o ~/.cargo/lib/$@ $(LOADABLE_MODULE_OPTIONS) $(CXXFLAGS) $(LDFLAGS) $^

# %.o:  $(SRC_DIR)/%.cpp
# 	@echo "Compiling $*.cpp"
# 	$(CXX) -c $(CPPFLAGS) $(CXXFLAGS) $<

# $(LCPASS): $(LCPASS_OBJECTS)
# 	@echo "Creating library $@"
# 	$(CXX) -o $@ $(LOADABLE_MODULE_OPTIONS) $(CXXFLAGS) $(LDFLAGS) $^
# 	rm -rf $(LCPASS_OBJECTS)

clean:
	rm -f $(LCPASS) $(LCPASS_OBJECTS)
