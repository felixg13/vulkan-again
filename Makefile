.PHONY: debug release clean test run help
SRC_DIRS := src
SRC_FILES := $(shell find $(SRC_DIRS) \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \))

debug:
	cmake -S . -B build/debug \
	  -DCMAKE_BUILD_TYPE=Debug \
	  -DCMAKE_CXX_FLAGS="-I/opt/vulkan-sdk/x86_64/include" \
	  -DCMAKE_EXE_LINKER_FLAGS="-L/opt/vulkan-sdk/x86_64/lib -lslang -Wl,-rpath,/opt/vulkan-sdk/x86_64/lib"
	cmake --build build/debug -j$$(nproc)

release:
	cmake -S . -B build/release \
	  -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=OFF -DENABLE_UBSAN=OFF \
	  -DCMAKE_CXX_FLAGS="-I/opt/1.4.321.1/x86_64/include" \
	  -DCMAKE_EXE_LINKER_FLAGS="-L/opt/1.4.321.1/x86_64/lib -lslang -Wl,-rpath,/opt/1.4.321.1/x86_64/lib"
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
