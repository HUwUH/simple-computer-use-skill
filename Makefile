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
    src/actions/act_screen.cpp \
    src/actions/act_mouse.cpp \
    src/actions/act_overlay.cpp \
    src/actions/act_kbd.cpp \
    src/actions/act_misc.cpp

all: bin/simple_cua.exe bin/cua_server.exe bin/cua_panic.exe

bin/simple_cua.exe: src/client.cpp $(COMMON)
	@if not exist bin mkdir bin
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

bin/cua_server.exe: src/server.cpp $(ACTIONS) $(COMMON)
	@if not exist bin mkdir bin
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS_SRV)

# 救火程序。故意保持极简：只依赖 winutil，不碰管道、不碰任何配置。
# 它必须由【人】用普通权限跑 —— 要杀一个 Medium 的进程，动手的也得是 Medium。
bin/cua_panic.exe: src/panic.cpp src/common/winutil.cpp
	@if not exist bin mkdir bin
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

clean:
	del /Q bin\*.exe
