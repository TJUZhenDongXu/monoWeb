# 变量定义
CXX := g++
CXXFLAGS := -Wall -g -std=c++20
LDFLAGS := -lpthread
TARGET := server
SRC := main.cpp

# 默认目标
all: $(TARGET)

# 编译目标
$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# 清理编译生成的文件
clean:
	rm -f $(TARGET)
