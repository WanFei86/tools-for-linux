# # 编译器
# CC = gcc

# # 编译选项
# CFLAGS = -Wall -Wextra -g -Iinc

# # 链接选项
# LDFLAGS = -lz

# # 目标文件目录
# OBJDIR = obj
# BINDIR = bin

# # 源文件
# SRCS = $(wildcard src/*.c)

# # 目标文件（.c -> .o）
# OBJS = $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRCS))

# # 最终生成的可执行文件
# TARGET = $(BINDIR)/server

# # 默认目标
# all: $(TARGET)

# # 链接
# $(TARGET): $(OBJS) | $(BINDIR)
# 	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# # 编译 .c -> .o
# $(OBJDIR)/%.o: src/%.c | $(OBJDIR)
# 	$(CC) $(CFLAGS) -c $< -o $@

# # 创建目录
# $(OBJDIR) $(BINDIR):
# 	mkdir -p $@

# # 清理
# clean:
# 	rm -rf $(OBJDIR) $(BINDIR)

# # 声明伪目标
# .PHONY: all clean

# 编译器
CC = gcc

# 编译选项
CFLAGS = -Wall -Wextra -g -Iinc -MMD -MP

# 链接选项
LDFLAGS = -lz

# 目录
SRCDIR = src
INCDIR = inc
BINDIR = bin

# 自动找出所有 .c 文件
SRCS = $(wildcard $(SRCDIR)/*.c)

# 生成对应的可执行文件名
# src/server.c -> bin/server
TARGETS = $(patsubst $(SRCDIR)/%.c,$(BINDIR)/%,$(SRCS))

# 默认目标：编译所有程序
all: $(BINDIR) $(TARGETS)

# 链接规则：bin/server 依赖 src/server.c
$(BINDIR)/%: $(SRCDIR)/%.c | $(BINDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

# 创建 bin 目录
$(BINDIR):
	mkdir -p $(BINDIR)

# 清理
clean:
	rm -rf $(BINDIR)
	find . -name "*.d" -delete

# 自动包含依赖文件（.h 改动会自动重编）
-include $(SRCS:.c=.d)

.PHONY: all clean