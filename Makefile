# ===========================================================================
# simple-computer-use  —  构建
#
# 用法：
#     mingw32-make          编译全部
#     mingw32-make clean    删除产物
#
# 注意：下面每条【命令行】必须用 TAB 开头，不能用空格 —— 这是 Makefile
#       最经典的坑，报错信息（missing separator）很难懂。
# ===========================================================================

SHELL = cmd.exe

CXX      = g++
CXXFLAGS = -std=c++17 -O2 -municode -static -Wall -Wextra

LIBS     = -lgdi32 -luser32 -ladvapi32
LIBS_SRV = $(LIBS) -lgdiplus

# 加文件时记得在这里加一行（故意列死，看得见）
COMMON  = \
    src/common/wire.cpp \
    src/common/winutil.cpp \
    src/common/winmon.cpp \
    src/common/pathutil.cpp

ACTIONS = \
    src/actions/actions.cpp \
    src/actions/act_state.cpp \
    src/actions/act_screen.cpp

all: bin/simple_cua.exe bin/cua_server.exe

bin/simple_cua.exe: src/client.cpp $(COMMON)
	@if not exist bin mkdir bin
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

bin/cua_server.exe: src/server.cpp $(ACTIONS) $(COMMON)
	@if not exist bin mkdir bin
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS_SRV)

clean:
	del /Q bin\*.exe
