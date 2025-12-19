#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE_EXTENDED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <locale.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ncurses.h>
#include <wchar.h>
#include <stdint.h>

#define MAP_WIDTH 45
#define MAP_HEIGHT 25
#define VIEW_RADIUS 4

#define TILE_EMPTY 0
#define TILE_WALL  1
#define TILE_FILE  2
#define TILE_TELE  3
#define TILE_ICE   4

#define ICON_WALL  "⬛"
#define ICON_EMPTY "  "
#define ICON_VAC   "💊"
#define ICON_VIR   "🦠"
#define ICON_FILE  "💾"
#define ICON_TELE  "🔮"
#define ICON_ICE   "🧊"
#define ICON_SNOW  "❄️"

#define MAX_FILES 4
#define LOG_MAX 15
#define MCAST_GRP "224.1.1.1"
#define MCAST_PORT 5000

typedef enum {
    MSG_DISCOVERY, MSG_REPORT_ROLE, MSG_MOVE,
    MSG_ITEM_TAKEN, MSG_FREEZE, MSG_GAME_OVER,
    MSG_FILE_MSG, MSG_TIME_SYNC
} MsgType;

typedef enum { ROLE_NONE, ROLE_VACCINE, ROLE_VIRUS, ROLE_OBSERVER } Role;

typedef struct {
    int sender_id;
    MsgType type;
    Role role;
    int x, y;
    int data;
} Packet;

pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

// 전역 변수
int sock_fd;
struct sockaddr_in mcast_addr;
int my_id;
Role my_role = ROLE_NONE;

int vac_x = 1, vac_y = 1;
int vir_x = MAP_WIDTH - 2, vir_y = MAP_HEIGHT - 2;
int obs_cam_x = MAP_WIDTH/2, obs_cam_y = MAP_HEIGHT/2;

int map[MAP_HEIGHT][MAP_WIDTH];
int visited[MAP_HEIGHT][MAP_WIDTH];

// 게임 상태
volatile int game_running = 1;
volatile int game_finished = 0;
int global_winner = 0; // 0:진행중, 1:백신승, 2:바이러스승
volatile int game_started = 0;
volatile time_t game_start_time = 0;

char msg_log[LOG_MAX][100];
int log_colors[LOG_MAX];

int found_vaccine = 0, found_virus = 0;
time_t last_vaccine_seen = 0, last_virus_seen = 0;

volatile int need_redraw = 1;
time_t freeze_until = 0;
int files_collected = 0;

// 알림 메시지 변수
char top_alert_msg[200] = "";
int top_alert_color = 0;
time_t top_alert_timer = 0;

time_t next_radar_time = 0;

char *file_msgs[] = {
    "시프_수업자료.zip 이(가) 삭제되었습니다.",
    "[ELEC0462-002]기말고사 문제.pdf 이(가) 삭제되었습니다.",
    "하숙생이_전부_미녀입니다만?2.exe 이(가) 삭제되었습니다.",
    "[ELEC0462-002]출튀_학생리스트.xsl 이(가) 삭제되었습니다."
};

// 함수 선언
void draw_ui();
void show_centered_msg(char *msg, int color);
void handle_game_over(int winner);

// 로그 시스템
void add_log(char *msg, int color_pair) {
    int elapsed = 0;
    if (game_start_time > 0) elapsed = time(NULL) - game_start_time;
    
    char full_msg[256];
    snprintf(full_msg, sizeof(full_msg), "[%02d:%02d] %s", elapsed / 60, elapsed % 60, msg);

    // 화면 오른쪽 패널 너비 계산
    int split_x = (COLS * 2) / 3;
    int max_width = COLS - split_x - 4;
    if (max_width < 5) max_width = 5;

    // 한글/이모지 처리를 위해 와이드 문자로 변환
    wchar_t wbuf[256];
    mbstowcs(wbuf, full_msg, 255);
    int len = wcslen(wbuf);

    int current_width = 0;
    int start_idx = 0;

    // 글자 하나하나 너비 체크하며 출력
    for (int i = 0; i < len; i++) {
        int char_width = wcwidth(wbuf[i]);
        if (char_width < 0) char_width = 1; // 특수문자 예외처리

        // 줄바꿈
        if (current_width + char_width > max_width) {
            for (int j = 0; j < LOG_MAX - 1; j++) {
                strcpy(msg_log[j], msg_log[j + 1]);
                log_colors[j] = log_colors[j + 1];
            }
            wchar_t temp = wbuf[i];
            wbuf[i] = L'\0';
            wcstombs(msg_log[LOG_MAX - 1], &wbuf[start_idx], 99);
            log_colors[LOG_MAX - 1] = color_pair;

            wbuf[i] = temp;
            start_idx = i;
            current_width = 0;
        }
        current_width += char_width;
    }
    for (int j = 0; j < LOG_MAX - 1; j++) {
        strcpy(msg_log[j], msg_log[j + 1]);
        log_colors[j] = log_colors[j + 1];
    }
    wcstombs(msg_log[LOG_MAX - 1], &wbuf[start_idx], 99);
    log_colors[LOG_MAX - 1] = color_pair;

    need_redraw = 1;
}

// 상단 알림 설정
void set_top_alert(char *msg, int color, int duration) {
    strncpy(top_alert_msg, msg, 199);
    top_alert_color = color;
    top_alert_timer = time(NULL) + duration;
    need_redraw = 1;
}

static uint32_t next = 1;

int my_rand(void) {
    next = next * 1103515245 + 12345;
    return (unsigned int)(next/65536) % 32768;
}

void my_srand(unsigned int seed) {
    next = seed;
}

// 미로 생성
int dx[] = {0, 0, -2, 2};
int dy[] = {-2, 2, 0, 0};

void shuffle_directions(int *array) {
    for (int i = 0; i < 4; i++) {
        int r = my_rand() % 4;
        int temp = array[i];
        array[i] = array[r];
        array[r] = temp;
    }
}

void carves_maze(int x, int y) {
    visited[y][x] = 1; map[y][x] = TILE_EMPTY;
    int dirs[] = {0, 1, 2, 3};
    shuffle_directions(dirs);
    
    for (int i = 0; i < 4; i++) {
        int nx = x + dx[dirs[i]];
        int ny = y + dy[dirs[i]];
        if (nx>0 && nx<MAP_WIDTH-1 && ny>0 && ny<MAP_HEIGHT-1 && !visited[ny][nx]) {
            map[y + dy[dirs[i]]/2][x + dx[dirs[i]]/2] = TILE_EMPTY;
            carves_maze(nx, ny);
        }
    }
}

void braid_maze() {
    for(int y=1; y<MAP_HEIGHT-1; y++) {
        for(int x=1; x<MAP_WIDTH-1; x++) {
            if(map[y][x]==TILE_WALL && my_rand()%100 < 30) {
                if(map[y-1][x]==0 && map[y+1][x]==0) map[y][x]=0;
                if(map[y][x-1]==0 && map[y][x+1]==0) map[y][x]=0;
            }
        }
    }
}

void place_object_randomly(int type, int count) {
    int placed=0;
    while(placed<count) {
        int rx=my_rand()%(MAP_WIDTH-2)+1;
        int ry=my_rand()%(MAP_HEIGHT-2)+1;
        if(map[ry][rx]==TILE_EMPTY) { map[ry][rx]=type; placed++; }
    }
}

void find_spawn_fixed() {
    // 백신 시작점 찾기
    for(int y=1; y<MAP_HEIGHT-1; y++) {
        for(int x=1; x<MAP_WIDTH-1; x++) {
            if(map[y][x] == TILE_EMPTY) { vac_x = x; vac_y = y; goto FIND_VIRUS; }
        }
    }
FIND_VIRUS:
    // 바이러스 시작점 찾기
    for(int y=MAP_HEIGHT-2; y>0; y--) {
        for(int x=MAP_WIDTH-2; x>0; x--) {
            if(map[y][x] == TILE_EMPTY) { vir_x = x; vir_y = y; return; }
        }
    }
}

void generate_map() {
    // 맵 동기화를 위해 시간을 시드로 사용 (300초 단위로 변경)
    unsigned int auto_seed = time(NULL) / 300;
    my_srand(auto_seed);

    for(int y=0; y<MAP_HEIGHT; y++)
        for(int x=0; x<MAP_WIDTH; x++) {
            map[y][x]=TILE_WALL;
            visited[y][x]=0;
        }

    carves_maze(1, 1);
    braid_maze();
    place_object_randomly(TILE_FILE, 4);
    place_object_randomly(TILE_TELE, 5);
    place_object_randomly(TILE_ICE, 5);
    find_spawn_fixed();

    for(int i=0; i<LOG_MAX; i++) { strcpy(msg_log[i], ""); log_colors[i]=0; }
    add_log("System Boot Sequence Initiated...", 0);
    
    // 게임 내 랜덤 이벤트는 개별 시드 사용
    srand(time(NULL) + getpid());
}

void send_packet(MsgType type, Role role, int x, int y, int data) {
    Packet pkt = {my_id, type, role, x, y, data};
    sendto(sock_fd, &pkt, sizeof(pkt), 0, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));
}

// 레이더 (백신)
void update_radar() {
    if (my_role != ROLE_VACCINE) return;
    time_t now = time(NULL);
    if (now >= next_radar_time) {
        double dy = (double)(vir_y - vac_y);
        double dx = (double)(vir_x - vac_x);
        double angle = atan2(dy, dx) * 180.0 / M_PI;
        
        char *dir_str = "Unknown";
        if (angle >= -22.5 && angle < 22.5) dir_str = "➡️ RIGHT";
        else if (angle >= 22.5 && angle < 67.5) dir_str = "↘️ DOWN-RIGHT";
        else if (angle >= 67.5 && angle < 112.5) dir_str = "⬇️ DOWN";
        else if (angle >= 112.5 && angle < 157.5) dir_str = "↙️ DOWN-LEFT";
        else if (angle >= 157.5 || angle < -157.5) dir_str = "⬅️ LEFT";
        else if (angle >= -157.5 && angle < -112.5) dir_str = "↖️ UP-LEFT";
        else if (angle >= -112.5 && angle < -67.5) dir_str = "⬆️ UP";
        else if (angle >= -67.5 && angle < -22.5) dir_str = "↗️ UP-RIGHT";
        
        char buf[50]; sprintf(buf, "RADAR: %s", dir_str);
        set_top_alert(buf, 3, 3);
        next_radar_time = now + 10;
        add_log("Radar Scan Complete.", 3);
    }
}

// 삭제 연출
void play_fake_deletion() {
    timeout(50);
    for(int i=0; i<80; i++) {
        int rx=rand()%COLS; int ry=rand()%LINES;
        int color=(rand()%2)?1:2;
        attron(COLOR_PAIR(color)); mvprintw(ry, rx, "%c", (rand()%2)?'1':'0'); attroff(COLOR_PAIR(color));
        refresh(); napms(30);
    }
    napms(300); clear();
    int box_w = 40; int sx = (COLS - box_w) / 2; int sy = LINES / 2 - 4;
    attron(COLOR_PAIR(1)|A_BOLD);
    mvprintw(sy, sx+10, "CRITICAL ERROR");
    mvprintw(sy+2, sx+8, "Deleting System32...");
    
    // 중앙 정렬 게이지
    for(int i=0; i<20; i++) {
        mvprintw(sy+4, sx + 5, "[");
        for(int j=0; j<20; j++) {
            if(j <= i) printw("█"); else printw(" ");
        }
        printw("] %d%%", (i+1)*5);
        refresh(); napms(100);
    }
    if (my_role == ROLE_VACCINE) mvprintw(sy+6, sx+5, "프로그램이 삭제되었습니다...");\
    else  mvprintw(sy+6, sx+5, "바이러스가 삭제되었습니다...");
    attroff(COLOR_PAIR(1)|A_BOLD);
}

void show_centered_msg(char *msg, int color) {
    clear();
    char *temp_msg = strdup(msg);
    if (temp_msg == NULL) return;

    int line_count = 0;
    char *count_ptr = strdup(msg);
    char *token = strtok(count_ptr, "\n");
    while (token != NULL) {
        line_count++;
        token = strtok(NULL, "\n");
    }
    free(count_ptr);

    int start_y = (LINES - line_count) / 2;
    attron(COLOR_PAIR(color) | A_BOLD);
    token = strtok(temp_msg, "\n");
    int current_y = start_y;

    while (token != NULL) {
        // 실제 화면 너비 계산
        wchar_t wbuf[200];
        // char* -> wchar_t*
        mbstowcs(wbuf, token, 199);
        int width = wcswidth(wbuf, 200);
        
        // 변환 실패 시 안전장치로 strlen 사용
        if (width < 0) width = strlen(token);

        int x = (COLS - width) / 2;
        if (x < 0) x = 0;

        mvprintw(current_y++, x, "%s", token);
        token = strtok(NULL, "\n");
    }

    attroff(COLOR_PAIR(color) | A_BOLD);
    refresh();
    free(temp_msg);
}

void show_story_sequence() {
    clear();
    attron(A_BOLD);
    
    // 제목 출력
    char *title = "=== GAME STORY ===";
    mvprintw(LINES/2 - 4, (COLS - strlen(title))/2, "%s", title);
    
    char *story = "";
    int color = 0;

    if (my_role == ROLE_VACCINE) {
        story = "컴퓨터에 침입한 바이러스를 잡아야한다!\n못잡으면 mhan이 당신을 삭제할 것.";
        color = 2; // Green
    } else if (my_role == ROLE_VIRUS) {
        story = "교수님의 시험 문제를 지워야한다.\n파일 4개를 찾아라.\n단, 백신한테 잡히면 내가 삭제된다.";
        color = 1; // Red
    } else {
        story = "관전 모드: 두 프로그램의 운명을 지켜보세요.";
        color = 3; // Yellow
    }
    
    attron(COLOR_PAIR(color));
    
    int line_off = 0;
    char temp[300];
    strcpy(temp, story);
    char *token = strtok(temp, "\n");

    while(token != NULL) {
        wchar_t wbuf[200];
        mbstowcs(wbuf, token, 199); // wide char 변환
        int width = wcswidth(wbuf, 200); // 실제 화면 너비 계산
        
        if (width < 0) width = strlen(token);

        int x = (COLS - width) / 2;
        if (x < 0) x = 0;

        mvprintw(LINES/2 + line_off - 1, x, "%s", token);
        
        token = strtok(NULL, "\n");
        line_off++;
    }
    attroff(COLOR_PAIR(color)); attroff(A_BOLD);
    
    // 카운트다운
    int wait_time = 10;
    for(int i=wait_time; i>0; i--) {
        char buf[50];
        sprintf(buf, "GAME STARTS IN %d...", i);
        move(LINES-3, 0); // 커서를 해당 줄 맨 앞으로 이동
        clrtoeol();
        mvprintw(LINES-3, (COLS - strlen(buf))/2, "%s", buf);
        refresh();
        napms(1000);
    }
}

void wait_for_exit() {
    timeout(-1);
    attron(A_BLINK|A_REVERSE);
    mvprintw(LINES-2, COLS/2-11, " PRESS ANY KEY TO EXIT ");
    attroff(A_BLINK|A_REVERSE);
    refresh(); flushinp(); getch();
}

void handle_game_over(int winner) {
    global_winner = winner; // 승자 기록
    game_running = 0;       // 메인 루프 종료
    game_finished = 1;      // 종료 상태로 전환
}

// 수신 스레드
void* recv_thread(void* arg) {
    Packet pkt;
    struct sockaddr_in sender_addr;
    socklen_t addrlen = sizeof(sender_addr);

    // 타임아웃 설정
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 0.1초 타임아웃
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    while(game_running) {
        // 타임아웃이 걸리면 -1 리턴, 에러 발생 -> 루프 계속
        if (recvfrom(sock_fd, &pkt, sizeof(pkt), 0, (struct sockaddr*)&sender_addr, &addrlen) < 0) {
            continue;
        }

        if (pkt.sender_id == my_id) continue;

        if (pkt.type == MSG_DISCOVERY) {
            if (my_role != ROLE_NONE) {
                send_packet(MSG_REPORT_ROLE, my_role, 0, 0, 0);
                int mx = (my_role==ROLE_VACCINE)?vac_x:vir_x;
                int my = (my_role==ROLE_VACCINE)?vac_y:vir_y;
                send_packet(MSG_MOVE, my_role, mx, my, 0);
            }

            if (my_role == ROLE_VACCINE && game_started) {
                int elapsed = time(NULL) - game_start_time;
                send_packet(MSG_TIME_SYNC, my_role, 0, 0, elapsed);
            }
        }
        else if (pkt.type == MSG_TIME_SYNC) {
            time_t estimated_start = time(NULL) - pkt.data;
            if (game_start_time == 0 || abs(game_start_time - estimated_start) > 1) {
                game_start_time = estimated_start;
                game_started = 1;
            }
        }
        else if (pkt.type == MSG_REPORT_ROLE) {
            if (pkt.role == ROLE_VACCINE) { found_vaccine=1; last_vaccine_seen=time(NULL); }
            if (pkt.role == ROLE_VIRUS) { found_virus=1; last_virus_seen=time(NULL); }
            need_redraw = 1;
        }
        else if (pkt.type == MSG_MOVE) {
            if (pkt.role == ROLE_VACCINE) { vac_x = pkt.x; vac_y = pkt.y; }
            if (pkt.role == ROLE_VIRUS) { vir_x = pkt.x; vir_y = pkt.y; }
            need_redraw = 1;
        }
        else if (pkt.type == MSG_ITEM_TAKEN) {
            pthread_mutex_lock(&map_lock);
            map[pkt.y][pkt.x] = TILE_EMPTY;
            
            if (pkt.data == TILE_FILE) {
                files_collected++; // 바이러스가 먹은 파일 수 동기화
                add_log("[WARNING] File Data Compromised!", 1);
            }
            else if (pkt.data == TILE_TELE) {
                add_log("Teleport item is used.", 3);
            }
            else if (pkt.data == TILE_ICE) {
                add_log("Ice item is used.", 3);
            }
            pthread_mutex_unlock(&map_lock);
            need_redraw = 1;
        }
        else if (pkt.type == MSG_FREEZE) {
             Role target = (pkt.role == ROLE_VACCINE) ? ROLE_VIRUS : ROLE_VACCINE;
             if (my_role == target) {
                freeze_until = time(NULL) + 3;
                set_top_alert(">> 얼음!!!!!!!!!!!! <<", 4, 3);
             }
        }
        else if (pkt.type == MSG_GAME_OVER) {
            global_winner = pkt.data;
            game_running = 0;
            game_finished = 1;
        }
    }
    return NULL;
}

// 화면 그리기
void draw_ui() {
    erase();
    int split_x = (COLS * 2) / 3;
    for(int y=0; y<LINES; y++) mvprintw(y, split_x, "│");

    // 상단 알림창
    if (time(NULL) < top_alert_timer) {
        attron(COLOR_PAIR(top_alert_color) | A_BOLD | A_REVERSE);
        int len = strlen(top_alert_msg);
        int start_x = split_x/2 - len/2;
        if (start_x < 1) start_x = 1;
        mvprintw(1, start_x, " %s ", top_alert_msg);
        attroff(COLOR_PAIR(top_alert_color) | A_BOLD | A_REVERSE);
    }
    else {
        attron(A_BOLD); mvprintw(1, split_x/2 - 8, "[ MAZE TAG ]"); attroff(A_BOLD);
    }

    // 오른쪽 UI
    attron(A_BOLD); mvprintw(1, split_x + 2, "=== STATUS ==="); attroff(A_BOLD);
    char role_str[100];
    if(my_role==ROLE_VACCINE) sprintf(role_str, "ROLE: VACCINE %s", ICON_VAC);
    else if(my_role==ROLE_VIRUS) sprintf(role_str, "ROLE: VIRUS %s", ICON_VIR);
    else sprintf(role_str, "ROLE: OBSERVER");
    mvprintw(3, split_x + 2, "%s", role_str);
    
    int elapsed = 0;
    if (game_start_time > 0) elapsed = time(NULL) - game_start_time;
    
    attron(A_BOLD);
    mvprintw(4, split_x + 2, "TIME: %02d:%02d", elapsed / 60, elapsed % 60);
    attroff(A_BOLD);

    if (my_role == ROLE_VIRUS || my_role == ROLE_OBSERVER) {
        mvprintw(5, split_x + 2, "FILE: %d / %d", files_collected, MAX_FILES);
    }
    if (my_role == ROLE_VACCINE) {
        int left = next_radar_time - time(NULL); if(left < 0) left = 0;
        mvprintw(5, split_x + 2, "바이러스 위치: %ds", left);
    }
    if (time(NULL) < freeze_until) {
        attron(COLOR_PAIR(1)|A_BLINK); mvprintw(7, split_x+2, "[ FROZEN ]"); attroff(COLOR_PAIR(1)|A_BLINK);
    } else {
        attron(COLOR_PAIR(2)); mvprintw(7, split_x+2, "[ ONLINE ]"); attroff(COLOR_PAIR(2));
    }

    mvprintw(9, split_x + 2, "--- LOG ---");
    for(int i=0; i<LOG_MAX; i++) {
        if (strlen(msg_log[i]) > 0) {
            attron(COLOR_PAIR(log_colors[i]));
            mvprintw(11 + i, split_x + 2, "> %s", msg_log[i]);
            attroff(COLOR_PAIR(log_colors[i]));
        }
    }

    if (!game_started) {
        mvprintw(LINES/2, split_x/2 - 10, "WAITING PLAYERS..."); refresh(); return;
    }

    int cam_x, cam_y;
    int rx, ry;

    int cx = split_x/2;
    int cy = LINES/2;

    if (my_role == ROLE_OBSERVER) {
        cam_x = obs_cam_x;
        cam_y = obs_cam_y;
        rx = 28;
        ry = 15;
    }
    else {
        cam_x = (my_role==ROLE_VACCINE)?vac_x:vir_x;
        cam_y = (my_role==ROLE_VACCINE)?vac_y:vir_y;
        rx = VIEW_RADIUS;
        ry = VIEW_RADIUS;
    }
    pthread_mutex_lock(&map_lock);
    for (int y = cam_y - ry; y <= cam_y + ry; y++) {
        for (int x = cam_x - rx; x <= cam_x + rx; x++) {
            
            // 화면 좌표 계산
            int sy = cy + (y - cam_y);
            int sx = cx + (x - cam_x) * 2;

            if (sx < 0 || sx >= split_x - 1 || sy < 0 || sy >= LINES) continue;
            
            // 맵 데이터 범위 체크
            if (y < 0 || y >= MAP_HEIGHT || x < 0 || x >= MAP_WIDTH) continue;

            int tile = map[y][x];
            
            if (tile == TILE_FILE && my_role == ROLE_VACCINE) mvprintw(sy, sx, "%s", ICON_EMPTY);
            else if (tile == TILE_WALL) mvprintw(sy, sx, "%s", ICON_WALL);
            else if (tile == TILE_FILE) mvprintw(sy, sx, "%s", ICON_FILE);
            else if (tile == TILE_TELE) mvprintw(sy, sx, "%s", ICON_TELE);
            else if (tile == TILE_ICE)  mvprintw(sy, sx, "%s", ICON_ICE);
            else mvprintw(sy, sx, "%s", ICON_EMPTY);

            if (x == vac_x && y == vac_y) mvprintw(sy, sx, "%s", ICON_VAC);
            if (x == vir_x && y == vir_y) mvprintw(sy, sx, "%s", ICON_VIR);
        }
    }
    pthread_mutex_unlock(&map_lock);
    // 얼음 효과
    if (time(NULL) < freeze_until) {
        attron(COLOR_PAIR(4) | A_BOLD);
        for(int x=0; x < split_x; x++) {
            mvprintw(0, x, ICON_SNOW);
            mvprintw(LINES-1, x, ICON_SNOW);
        }
        for(int y=0; y < LINES; y++) {
            mvprintw(y, 0, ICON_SNOW);
            mvprintw(y, split_x - 2, ICON_SNOW);
        }
        attroff(COLOR_PAIR(4) | A_BOLD);
    }
    refresh();
}

// MAZE TAG
void draw_title_art(int start_y) {

    int letters[7][5][5] = {
        { // M
            {1,0,0,0,1},
            {1,1,0,1,1},
            {1,0,1,0,1},
            {1,0,0,0,1},
            {1,0,0,0,1}
        },
        { // A
            {0,1,1,1,0},
            {1,0,0,0,1},
            {1,1,1,1,1},
            {1,0,0,0,1},
            {1,0,0,0,1}
        },
        { // Z
            {1,1,1,1,1},
            {0,0,0,1,0},
            {0,0,1,0,0},
            {0,1,0,0,0},
            {1,1,1,1,1}
        },
        { // E
            {1,1,1,1,1},
            {1,0,0,0,0},
            {1,1,1,1,0},
            {1,0,0,0,0},
            {1,1,1,1,1}
        },
        { // T
            {1,1,1,1,1},
            {0,0,1,0,0},
            {0,0,1,0,0},
            {0,0,1,0,0},
            {0,0,1,0,0}
        },
        { // A
            {0,1,1,1,0},
            {1,0,0,0,1},
            {1,1,1,1,1},
            {1,0,0,0,1},
            {1,0,0,0,1}
        },
        { // G
            {0,1,1,1,1},
            {1,0,0,0,0},
            {1,0,1,1,1},
            {1,0,0,0,1},
            {0,1,1,1,0}
        }
    };
    int total_width = (7 * 5 + 6) * 2;
    int start_x = (COLS - total_width) / 2;
    if (start_x < 0) start_x = 0;

    attron(COLOR_PAIR(2));
    
    for(int row=0; row<5; row++) {
        int current_x = start_x;
        for(int l=0; l<7; l++) {
            for(int col=0; col<5; col++) {
                if(letters[l][row][col]) mvprintw(start_y + row, current_x, "%s", ICON_WALL);
                current_x += 2;
            }
            current_x += 2;
            if(l == 3) current_x += 4;
        }
    }
    attroff(COLOR_PAIR(2));
}

// 도움말 & 연습 모드
void show_help_screen() {
    // 연습용 미니맵(15x20)
    // 0:길, 1:벽, 2:파일, 3:텔포, 4:얼음
    int p_h = 13;
    int p_w = 22;
    int p_map[13][22] = {
        {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
        {1,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
        {1,0,0,1,1,0,1,0,1,1,1,1,1,0,1,1,1,0,1,1,0,1},
        {1,0,1,2,1,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,1},
        {1,0,1,0,1,1,1,1,1,0,1,1,1,1,1,0,1,1,1,1,0,1},
        {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
        {1,0,1,1,0,1,0,1,1,1,1,1,0,1,0,1,1,0,1,1,0,1},
        {1,0,0,0,0,1,0,0,0,4,0,0,0,1,0,0,0,0,0,0,0,1},
        {1,0,1,1,1,1,0,1,1,1,1,1,0,1,1,1,1,0,1,1,0,1},
        {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,1},
        {1,0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,0,1},
        {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
        {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
    };

    int px = 2, py = 2;
    char practice_log[100] = "방향키를 눌러 움직여주세요!";
    int p_msg_color = 0;

    while(1) {
        erase();
        int split_x = (COLS * 2) / 3;
        
        for(int y=0; y<LINES; y++) mvprintw(y, split_x, "│");
        
        int tx = split_x + 2;
        int ty = 1;

        attron(A_BOLD | COLOR_PAIR(3));
        mvprintw(ty++, tx, "=== HOW TO PLAY ===");
        attroff(A_BOLD | COLOR_PAIR(3));
        ty++;

        // 역할 설명
        attron(A_BOLD); mvprintw(ty++, tx, "[ 역할 ]"); attroff(A_BOLD);
        attron(COLOR_PAIR(2)); mvprintw(ty++, tx, "1. VACCINE %s (술래)", ICON_VAC); attroff(COLOR_PAIR(2));
        mvprintw(ty++, tx, "   - 바이러스를 잡으면 승리.");
        mvprintw(ty++, tx, "   - 10초마다 바이러스의 위치(방향)를 알 수 있습니다.");
        ty++;
        attron(COLOR_PAIR(1)); mvprintw(ty++, tx, "2. VIRUS %s (도망자)", ICON_VIR); attroff(COLOR_PAIR(1));
        mvprintw(ty++, tx, "   - 4개의 파일을 찾아 삭제(획득)하면 승리");
        mvprintw(ty++, tx, "   - 파일을 모두 모으기 전 백신에게 잡히면 패배입니다.");
        ty++;

        // 아이템 설명
        attron(A_BOLD); mvprintw(ty++, tx, "[ 아이템 ]"); attroff(A_BOLD);
        mvprintw(ty++, tx, "%s FILE : 바이러스의 목표이며 백신에게 보이지 않습니다.", ICON_FILE);
        mvprintw(ty++, tx, "%s TELEPORT : 미로 내 랜덤한 위치로 이동합니다.", ICON_TELE);
        mvprintw(ty++, tx, "%s ICE : 상대를 3초간 정지시킵니다.", ICON_ICE);
        ty++;

        // 승리 조건
        attron(A_BOLD); mvprintw(ty++, tx, "[ 승리 조건 ]"); attroff(A_BOLD);
        mvprintw(ty++, tx, "* VACCINE : 바이러스를 잡으면 승리");
        mvprintw(ty++, tx, "* VIRUS   : 4개의 파일을 찾아 획득하면 승리");
        
        attron(A_REVERSE | A_BLINK);
        mvprintw(LINES-3, split_x + 4, " PRESS [Q] TO BACK ");
        attroff(A_REVERSE | A_BLINK);

        int cx = split_x / 2;
        int cy = LINES / 2;
        
        // 타이틀
        attron(A_BOLD);
        mvprintw(cy - 9, cx - 8, "[ 연습 모드 ]");
        attroff(A_BOLD);

        // 맵 그리기
        int start_map_x = cx - p_w;
        int start_map_y = cy - p_h/2;

        for(int y=0; y<p_h; y++) {
            for(int x=0; x<p_w; x++) {
                int sx = start_map_x + (x*2);
                int sy = start_map_y + y;
                
                if (p_map[y][x] == 1) mvprintw(sy, sx, "%s", ICON_WALL);
                else if (p_map[y][x] == 2) mvprintw(sy, sx, "%s", ICON_FILE);
                else if (p_map[y][x] == 3) mvprintw(sy, sx, "%s", ICON_TELE);
                else if (p_map[y][x] == 4) mvprintw(sy, sx, "%s", ICON_ICE);
                else mvprintw(sy, sx, "%s", ICON_EMPTY);
            }
        }
        
        attron(A_BOLD | COLOR_PAIR(2));
        mvprintw(start_map_y + py, start_map_x + (px*2), "%s", ICON_VAC);
        attroff(A_BOLD | COLOR_PAIR(2));

        attron(COLOR_PAIR(p_msg_color) | A_BOLD);
        mvprintw(cy + 8, cx - strlen(practice_log)/2, "%s", practice_log);
        attroff(COLOR_PAIR(p_msg_color) | A_BOLD);


        refresh();

        int ch = getch();
        if (ch == 'q' || ch == 27) break; // 나가기

        // 이동 처리
        if (ch != ERR) {
            int nx = px;
            int ny = py;
            
            switch(ch) {
                case KEY_UP: ny--; break;
                case KEY_DOWN: ny++; break;
                case KEY_LEFT: nx--; break;
                case KEY_RIGHT: nx++; break;
            }

            // 벽 충돌 체크
            if (p_map[ny][nx] != 1) {
                px = nx;
                py = ny;
                p_msg_color = 0;

                // 아이템
                int tile = p_map[ny][nx];
                
                if (tile == 2) { // 파일
                    p_map[ny][nx] = 0;
                    sprintf(practice_log, "강의 잘하는 방법 10가지.txt 이(가) 삭제되었습니다.");
                    p_msg_color = 2;
                }
                else if (tile == 3) { // 텔레포트
                    p_map[ny][nx] = 0;
                    while(1) {
                        int rx = rand() % (p_w-2) + 1;
                        int ry = rand() % (p_h-2) + 1;
                        if(p_map[ry][rx] == 0) {
                            px = rx; py = ry; break;
                        }
                    }
                    sprintf(practice_log, "TELEPORTED!");
                    p_msg_color = 3;
                }
                else if (tile == 4) { // 얼음
                    p_map[ny][nx] = 0;
                    sprintf(practice_log, "얼음!!!!!!!!!!!!");
                    p_msg_color = 3;
                }
                else {
                    sprintf(practice_log, "Moving...");
                }
            }
        }
    }
    clear();
}

void select_and_wait() {
    clear();
    int choice = 0;
    char warning_msg[100] = "";
    int warning_timer = 0;

    mvprintw(LINES/2, COLS/2-5, "Scanning..."); refresh();
    for(int i=0; i<3; i++) { send_packet(MSG_DISCOVERY, ROLE_NONE, 0, 0, 0); usleep(100000); }

    while(1) {
        int vac_on = (time(NULL) - last_vaccine_seen < 3 && found_vaccine);
        int vir_on = (time(NULL) - last_virus_seen < 3 && found_virus);

        erase();
        draw_title_art(LINES/2 - 12);

        int menu_y = LINES/2 + 2;
        int center_x = COLS/2;
        int menu_width = 30;
        int start_x = center_x - (menu_width / 2);

        char *menus[4] = {"VACCINE", "VIRUS", " OBSERVER", "HOW TO PLAY"};
        char *icons[4] = {ICON_VAC, ICON_VIR, "👁️", "❓"};
        
        for(int i=0; i<4; i++) {
            int is_selected = (i == choice);
            int is_disabled = 0;
            if (i==0 && vac_on) is_disabled = 1;
            if (i==1 && vir_on) is_disabled = 1;

            if (is_selected) attron(A_REVERSE | A_BOLD);
            if (is_disabled) attron(COLOR_PAIR(1));

            char label[60];
            if (i == 3) {
                sprintf(label, "%s %s  %-10s", is_selected ? "▶" : " ", icons[i], menus[i]);
            } else {
                if (is_disabled) sprintf(label, "  %s  %-10s (OCCUPIED)", icons[i], menus[i]);
                else sprintf(label, "%s %s  %-10s", is_selected ? "▶" : " ", icons[i], menus[i]);
            }
            
            mvprintw(menu_y + (i*2), start_x, "%s", label);

            if (is_disabled) attroff(COLOR_PAIR(1));
            if (is_selected) attroff(A_REVERSE | A_BOLD);
        }

        mvprintw(menu_y + 9, center_x - 12, "Use [UP/DOWN] & [ENTER]");

        if (warning_timer > 0) {
            attron(COLOR_PAIR(1)|A_BOLD);
            mvprintw(menu_y + 11, center_x - strlen(warning_msg)/2, "%s", warning_msg);
            attroff(COLOR_PAIR(1)|A_BOLD);
            warning_timer--;
        }

        refresh();

        int ch = getch();
        if (ch != ERR) {
            if (ch == KEY_UP) {
                choice--;
                if (choice < 0) choice = 3;
            }
            else if (ch == KEY_DOWN) {
                choice++;
                if (choice > 3) choice = 0;
            }
            else if (ch == 10) { // Enter Key
                if (choice == 3) {
                    show_help_screen();
                }
                else if (choice == 0) {
                    if (vac_on) { strcpy(warning_msg, "!!! ROLE ALREADY TAKEN !!!"); warning_timer = 20; }
                    else { my_role = ROLE_VACCINE; break; }
                }
                else if (choice == 1) {
                    if (vir_on) { strcpy(warning_msg, "!!! ROLE ALREADY TAKEN !!!"); warning_timer = 20; }
                    else { my_role = ROLE_VIRUS; break; }
                }
                else {
                    my_role = ROLE_OBSERVER; break;
                }
            }
        }

        static int net_tick = 0;
        if (net_tick++ > 5) { send_packet(MSG_DISCOVERY, ROLE_NONE, 0, 0, 0); net_tick = 0; }
    }

    clear();
    while(!game_started) {
        send_packet(MSG_REPORT_ROLE, my_role, 0, 0, 0);
        
        int v1 = (time(NULL)-last_vaccine_seen<3) || (my_role==ROLE_VACCINE);
        int v2 = (time(NULL)-last_virus_seen<3) || (my_role==ROLE_VIRUS);
        
        if(v1 && v2) game_started = 1;

        erase();
        draw_title_art(LINES/2 - 12);
        
        attron(A_BLINK);
        mvprintw(LINES/2 + 2, COLS/2 - 10, "WAITING FOR PLAYERS...");
        attroff(A_BLINK);

        int info_x = COLS/2 - 10;
        mvprintw(LINES/2 + 5, info_x, "VACCINE : [%s]", v1 ? "READY" : "...");
        mvprintw(LINES/2 + 6, info_x, "VIRUS   : [%s]", v2 ? "READY" : "...");
        
        refresh();

        int ch = getch();
        if (ch == 'q') {
            endwin();
            exit(0);
        }
    }

    show_story_sequence();

    if (game_start_time == 0) {
        game_start_time = time(NULL);
    }
    
    add_log("Game Started.", 2);
    next_radar_time = time(NULL) + 10;
}

void init_network() {
    srand(time(NULL)); my_id = rand();
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);

    int reuse=1;
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=htonl(INADDR_ANY);
    addr.sin_port=htons(MCAST_PORT);
    
    if(bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Bind failed"); exit(1);
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr=inet_addr(MCAST_GRP);
    mreq.imr_interface.s_addr=htonl(INADDR_ANY);
    setsockopt(sock_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    
    int loop=1;
    setsockopt(sock_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family=AF_INET;
    mcast_addr.sin_addr.s_addr=inet_addr(MCAST_GRP);
    mcast_addr.sin_port=htons(MCAST_PORT);
}

// 메인 함수
int main() {
    setlocale(LC_ALL, "");
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    start_color();
    init_pair(1, COLOR_RED, COLOR_BLACK);
    init_pair(2, COLOR_GREEN, COLOR_BLACK);
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);
    init_pair(4, COLOR_CYAN, COLOR_BLUE);

    timeout(50);
    generate_map();
    init_network();

    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread, NULL);

    select_and_wait();
    static time_t last_ui_update = 0;

    // 메인 루프
    while(game_running) {
        int ch = getch();

        time_t now = time(NULL);
        if (now != last_ui_update) {
            last_ui_update = now;
            need_redraw = 1;
        }

        if (freeze_until > 0 && time(NULL) >= freeze_until) {
            freeze_until = 0;
            need_redraw = 1;
        }

        if(ch=='q') break;

        if(my_role!=ROLE_OBSERVER && time(NULL)<freeze_until) ch=ERR;

        update_radar();

        if(ch!=ERR) {
            // 관전자 이동
            if (my_role == ROLE_VACCINE && game_started) {
                static time_t last_sync = 0;
                
                if (now - last_sync >= 2) {
                    int elapsed = now - game_start_time;
                    send_packet(MSG_TIME_SYNC, my_role, 0, 0, elapsed);
                    last_sync = now;
                }
            }
            if(my_role==ROLE_OBSERVER) {
                switch(ch) {
                    case KEY_UP: if(obs_cam_y>0) obs_cam_y--; break;
                    case KEY_DOWN: if(obs_cam_y<MAP_HEIGHT-1) obs_cam_y++; break;
                    case KEY_LEFT: if(obs_cam_x>0) obs_cam_x--; break;
                    case KEY_RIGHT: if(obs_cam_x<MAP_WIDTH-1) obs_cam_x++; break;
                } need_redraw=1;
            } else {
                // 플레이어 이동
                int nx=(my_role==ROLE_VACCINE)?vac_x:vir_x;
                int ny=(my_role==ROLE_VACCINE)?vac_y:vir_y;
                switch(ch) {
                    case KEY_UP: ny--; break; case KEY_DOWN: ny++; break;
                    case KEY_LEFT: nx--; break; case KEY_RIGHT: nx++; break;
                }

                if(map[ny][nx]!=TILE_WALL) {
                    if(my_role==ROLE_VACCINE) { vac_x=nx; vac_y=ny; } else { vir_x=nx; vir_y=ny; }
                    int tile = map[ny][nx];
                    
                    // 아이템 처리
                    if(tile==TILE_TELE) {
                        map[ny][nx] = TILE_EMPTY;
                        send_packet(MSG_ITEM_TAKEN, my_role, nx, ny, TILE_TELE);
                        add_log("TELEPORTED!", 3);
                        int tx, ty;
                        while(1) { tx=rand()%(MAP_WIDTH-2)+1; ty=rand()%(MAP_HEIGHT-2)+1; if(map[ty][tx]==TILE_EMPTY) break; }
                        if(my_role==ROLE_VACCINE) { vac_x=tx; vac_y=ty; } else { vir_x=tx; vir_y=ty; }
                        set_top_alert("TELEPORTED!", 3, 2);
                    } else if(tile==TILE_ICE) {
                        map[ny][nx]=TILE_EMPTY;
                        send_packet(MSG_ITEM_TAKEN, my_role, nx, ny, TILE_ICE);
                        send_packet(MSG_FREEZE, my_role, 0, 0, 0);
                        add_log("얼음!!!!!!!!!!!!", 2);
                        set_top_alert("얼음!!!!!!!!!!!!", 2, 2);
                    } else if(tile==TILE_FILE && my_role==ROLE_VIRUS) {
                        map[ny][nx]=TILE_EMPTY;
                        files_collected++;
                        send_packet(MSG_ITEM_TAKEN, my_role, nx, ny, TILE_FILE);
                        // 파일별 멘트 출력 (1,2,3번째)
                        if(files_collected <= 4) {
                            set_top_alert(file_msgs[files_collected-1], 2, 5);
                            add_log(file_msgs[files_collected-1],2);
                            draw_ui();
                    
                            if(files_collected == 4) {
                                napms(1000);
                            }
                        }

                        if(files_collected>=MAX_FILES) {
                            send_packet(MSG_GAME_OVER, my_role, 0, 0, 2);
                            handle_game_over(2);
                        }
                    }
                    // 이동 및 잡기 판정
                    send_packet(MSG_MOVE, my_role, (my_role==ROLE_VACCINE?vac_x:vir_x), (my_role==ROLE_VACCINE?vac_y:vir_y), 0);
                    if(vac_x==vir_x && vac_y==vir_y) {
                        send_packet(MSG_GAME_OVER, my_role, 0, 0, 1); // 1 = 백신 승리
                        handle_game_over(1);
                    }
                    
                    need_redraw=1;
                }
            }
        }
        if(need_redraw) { draw_ui(); need_redraw=0; }
    }

    // 게임 종료 후 처리
    pthread_join(tid, NULL); // 수신 스레드 종료 대기

    napms(500); // 마지막 패킷 전송 대기
    if (global_winner == 1) { // 백신 승리
        if (my_role == ROLE_VACCINE) show_centered_msg("축하합니다. mhan이(가) 다음 달에도 백신을 결제합니다.", 2);
        else if (my_role == ROLE_VIRUS) {
            show_centered_msg("백신이 당신을 삭제했습니다.", 1);
            napms(3000); play_fake_deletion();
        }
        else show_centered_msg("mhan은(는) 파일을 지켰습니다.", 3);
    }
    else if (global_winner == 2) { // 바이러스 승리
        if (my_role == ROLE_VIRUS) {
            show_centered_msg("침투 성공.\n백신은 이것을 기억할 것입니다.", 2);
        }

        else if (my_role == ROLE_VACCINE) {
            show_centered_msg("mhan이(가) 백신을 삭제했습니다.", 1);
            napms(3000); play_fake_deletion();
        }
        else show_centered_msg("mhan은(는) 파일을 지키지 못했습니다.", 3);
    }
    
    wait_for_exit();
    endwin();
    close(sock_fd);
    return 0;
}
