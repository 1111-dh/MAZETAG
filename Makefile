# 1. 컴파일러 설정
CC = gcc
CFLAGS = -Wall -g
TARGET = game

# 2. 운영체제(OS) 감지
# 쉘 명령 'uname -s'를 실행해서 결과를 변수에 저장 (Linux 또는 Darwin)
UNAME_S := $(shell uname -s)

# 3. 라이브러리 설정 (조건문)

# 일단 기본값을 리눅스용(-lncursesw)으로 설정
LDLIBS = -lncursesw -lpthread -lm

# 만약 OS가 맥(Darwin)이라면, 라이브러리를 맥용(-lncurses)으로 덮어씀
ifeq ($(UNAME_S),Darwin)
    LDLIBS = -lncurses -lpthread -lm
endif

# 4. 빌드 규칙
all: $(TARGET)

# 주의: $(CC) 앞에는 반드시 [Tab] 키로 들여쓰기 해야 합니다.
$(TARGET): game.c
	$(CC) $(CFLAGS) -o $(TARGET) game.c $(LDLIBS)

clean:
	rm -f $(TARGET)
