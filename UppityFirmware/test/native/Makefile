# Host-side tests for the pure core and the mechanism simulator. No Arduino, no board.
#   make test          build and run everything (an undefined-behaviour report fails the run)
#   make test F=home   run only tests whose name contains "home"
#   make asan          same, with AddressSanitizer (deadlocks in ASan's own init on some
#                      macOS builds — Darwin 25 with Apple clang 17 — hence not the default)
CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -g -O1 -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
SRCS = test_main.cpp test_core.cpp test_sim.cpp test_motion.cpp test_rotary.cpp test_composite.cpp test_serial.cpp test_command.cpp test_net.cpp test_rescue.cpp test_commission.cpp test_review.cpp test_seek.cpp test_mesh.cpp ../../src/core/MotionSupervisor.cpp
HDRS = $(wildcard ../../src/core/*.h) $(wildcard ../../src/sim/*.h) uppity_test.h rig.h
BIN = uppity_tests

.PHONY: test asan clean
test: $(BIN)
	./$(BIN) $(F)
$(BIN): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)
asan: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -fsanitize=address -o $(BIN)-asan $(SRCS) && ./$(BIN)-asan $(F)
clean:
	rm -rf $(BIN) $(BIN)-asan $(BIN).dSYM $(BIN)-asan.dSYM
