.PHONY: debug release clean test run help
SRC_DIRS := src
SRC_FILES := $(shell find $(SRC_DIRS) \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \))

debug:
	cmake -S . -B build/debug \
	  -DCMAKE_BUILD_TYPE=Debug
	cmake --build build/debug -j$$(nproc)

release:
	cmake -S . -B build/release \
	  -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=OFF -DENABLE_UBSAN=OFF
	cmake --build build/release -j$$(nproc)

tidy: compile_commands.json
	@echo "Running clang-tidy..."
	clang-tidy -p build/debug \
	  -header-filter=^$(PWD)/src/ \
	  $(shell find src -name '*.cpp')

format:
	@echo "Formatting source files..."
	clang-format -i $(SRC_FILES)

clean:
	rm -rf build
