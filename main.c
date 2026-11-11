#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*
 * ขนาดและรูปทรงของสนาม:
 * FIELD_WIDTH/FIELD_HEIGHT คือจำนวนคอลัมน์/แถวของพื้นที่เล่น ไม่รวมกรอบ
 * ท่อแนวตั้งมีความกว้าง PIPE_WIDTH และช่องผ่านสูง GAP_HEIGHT
 * ท่อแนวนอนมีความสูง HORIZONTAL_PIPE_HEIGHT และมีช่องผ่านตามจำนวน/ความกว้างที่กำหนด
 * HORIZONTAL_PIPE_SPACING คำนวณระยะห่างแนวดิ่งเป็น 3/4 ของความสูงสนาม
 */
#define FIELD_WIDTH 72
#define FIELD_HEIGHT 28
#define PIPE_WIDTH 5
#define GAP_HEIGHT 6
#define PIPE_COUNT 3
#define HORIZONTAL_PIPE_COUNT 2
#define HORIZONTAL_PIPE_HEIGHT 2
#define HORIZONTAL_PIPE_SPACING (FIELD_HEIGHT * 3 / 4)
#define HORIZONTAL_GAP_COUNT 2
#define HORIZONTAL_GAP_WIDTH 17
#define HORIZONTAL_GAP_SEPARATION 3

/*
 * ตำแหน่งเริ่มต้นของนกอยู่ใกล้ด้านซ้ายและกึ่งกลางสนาม
 * FRAME_MILLISECONDS เป็นเวลารออินพุตต่อเฟรม; ค่าประมาณ 33 ms ให้ได้ราว 30 FPS
 * ตัวเลข KEY_LEFT/KEY_RIGHT เป็นรหัสภายในที่ read_key ใช้แทนปุ่มทิศทาง
 */
#define BIRD_X 12
#define BIRD_Y (FIELD_HEIGHT / 2)
#define FRAME_MILLISECONDS 33
#define PIPE_MOVE_EVERY 2
#define HORIZONTAL_PIPE_MOVE_EVERY 8
#define KEY_LEFT 256
#define KEY_RIGHT 257

/*
 * GRAVITY เพิ่มความเร็วลงเล็กน้อยทุกเฟรม
 * FLAP_SPEED ถูกนำไปลบจาก velocity จึงทำให้นกเคลื่อนที่ขึ้นทันทีเมื่อกด Space
 */
#define GRAVITY 0.027
#define FLAP_SPEED 0.5

/*
 * สถานะของท่อแนวตั้ง:
 * x คือคอลัมน์ซ้ายสุด, gap_top คือแถวบนสุดของช่องว่าง
 * scored ป้องกันไม่ให้ท่อเดิมเพิ่มคะแนนซ้ำระหว่างที่ยังผ่านนกอยู่
 */
typedef struct {
    int x;
    int gap_top;
    bool scored;
} Pipe;

/*
 * สถานะของท่อแนวนอน:
 * y คือแถวบนสุดของท่อ และ gap_left เก็บคอลัมน์เริ่มต้นของแต่ละช่องผ่าน
 * scored มีหน้าที่เหมือนใน Pipe คือให้คะแนนได้ครั้งเดียวต่อการผ่านท่อหนึ่งชุด
 */
typedef struct {
    int y;
    int gap_left[HORIZONTAL_GAP_COUNT];
    bool scored;
} HorizontalPipe;

/*
 * original เก็บการตั้งค่าเทอร์มินัลก่อนเข้าเกม
 * active บอกว่าได้เปลี่ยนโหมดแล้วหรือไม่ เพื่อไม่คืนค่าซ้ำโดยไม่จำเป็น
 */
typedef struct {
    struct termios original;
    bool active;
} Terminal;

/* ใช้สถานะร่วมกันระหว่างการตั้งค่าและการคืนค่าเทอร์มินัล */
static Terminal terminal;

/*
 * คืนค่าการตั้งค่าเดิมและแสดงเคอร์เซอร์ก่อนออกจากเกม
 * ฟังก์ชันนี้เรียกได้หลายครั้งอย่างปลอดภัย เพราะทำงานเฉพาะเมื่อ active เป็น true
 */
static void restore_terminal()
{
    if (terminal.active) {
        /* TCSAFLUSH รอให้การเปลี่ยนโหมดมีผล และทิ้งอินพุตค้างในคิว */
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal.original);
        /* คืนเคอร์เซอร์ ล้างรูปแบบสี และขึ้นบรรทัดใหม่ */
        write(STDOUT_FILENO, "\033[?25h\033[0m\n",
                    sizeof("\033[?25h\033[0m\n") - 1);
        terminal.active = false;
    }
}

/*
 * เตรียมเทอร์มินัลสำหรับเกมแบบรับปุ่มทันที:
 * - ตรวจว่า stdin/stdout เชื่อมต่อกับเทอร์มินัลจริง
 * - สำรองโหมดเดิม แล้วปิด echo และ canonical mode
 * - ตั้งค่าอ่านแบบไม่รออักขระครบชุด เพื่อให้เกมยังอัปเดตเฟรมเมื่อไม่มีปุ่มกด
 * คืน false เมื่อใช้เทอร์มินัลไม่ได้หรือเปลี่ยนโหมดไม่สำเร็จ
 */
static bool setup_terminal()
{
    struct termios raw;

    /* เกมต้องใช้เทอร์มินัลจริงทั้งทางรับและแสดงผล */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "Please run this game in a terminal.\n");
        return false;
    }
    if (tcgetattr(STDIN_FILENO, &terminal.original) == -1) {
        perror("tcgetattr");
        return false;
    }

    /*
     * ECHO ไม่แสดงปุ่มที่กดบนจอ; ICANON ไม่รอ Enter ก่อนส่งข้อมูลให้โปรแกรม
     * ปิด IXON เพื่อให้ Ctrl-S/Ctrl-Q ไม่หยุดการแสดงผล และ ICRNL เพื่อไม่แปลง CR
     * VMIN/VTIME เป็นศูนย์ทำให้ read() กลับมาทันทีเมื่อไม่มีข้อมูลพร้อมอ่าน
     */
    raw = terminal.original;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON);
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr");
        return false;
    }

    /*
     * ลงทะเบียน atexit เป็นการป้องกันกรณีออกจาก main ตามปกติ
     * การเรียก restore_terminal() จาก main อีกครั้งยังปลอดภัยเพราะเช็ก active
     */
    terminal.active = true;
    atexit(restore_terminal);
    write(STDOUT_FILENO, "\033[?25l", 6);
    return true;
}

/*
 * อ่านเหตุการณ์คีย์บอร์ดหนึ่งครั้ง:
 * - timeout_ms < 0: select() รอจนกว่าจะมีข้อมูล
 * - timeout_ms >= 0: รอได้ไม่เกินเวลาที่กำหนด แล้วคืน -1 เมื่อหมดเวลา
 * - คืนค่าปุ่ม ASCII โดยตรง หรือรหัส KEY_* สำหรับปุ่มที่ต้องแปลง
 * - คืน -1 หาก select/read ไม่ได้ข้อมูล
 */
static int read_key(int timeout_ms)
{
    fd_set input;  // fd_set ระบุว่าเราสนใจ stdin; select() จะเขียนผลกลับใน input
    struct timeval timeout;  // timeout ถูกแยกเป็นวินาทีและไมโครวินาทีตามชนิด struct timeval
    int result;  // result ของ select() หรือ read()
    unsigned char key;  // อ่านไบต์แรกของเหตุการณ์คีย์บอร์ด

    /*
     * FD_ZERO() ล้างชุด input ให้เป็นชุดว่าง แล้ว FD_SET() ใส่ STDIN_FILENO เข้าไป 
     * เพื่อบอกว่าเราต้องการตรวจว่ามีข้อมูลให้อ่านจากช่องรับข้อมูลมาตรฐานหรือไม่
     */
    FD_ZERO(&input);
    FD_SET(STDIN_FILENO, &input);

    /*
     * select() รอจนกว่าจะมีข้อมูลให้อ่านหรือหมดเวลา:
     * - timeout_ms < 0: รอไม่จำกัดเวลา
     * - timeout_ms >= 0: แปลงเป็น struct timeval แล้วรอได้ไม่เกินเวลาที่กำหนด
     * 
     * เมื่อ select() บอกว่าอ่านได้แล้ว 
     * read() จึงอ่านข้อมูลจาก stdin จำนวน 1 ไบต์มาเก็บใน key 
     * ถ้าอ่านไม่ได้ครบหนึ่งไบต์ก็คืน -1
     */ 
    if (timeout_ms < 0) {
        result = select(STDIN_FILENO + 1, &input, NULL, NULL, NULL);
    } else {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        result = select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout);
    }

    if (result <= 0) {
        return -1;
    }
    if (read(STDIN_FILENO, &key, 1) != 1) {
        return -1;
    }

    /*
     * เทอร์มินัลส่วนใหญ่ส่งลูกศรเป็น escape sequence สามไบต์ เช่น ESC [ D
     * อ่านไบต์ที่ตามมาด้วยช่วงรอสั้น ๆ เพราะทั้งชุดควรเข้ามาต่อเนื่องกัน
     */
    if (key == '\033') {
        int bracket = read_key(10);
        int direction = read_key(10);
        if (bracket == '[' && direction == 'D') {
            return KEY_LEFT;
        }
        if (bracket == '[' && direction == 'C') {
            return KEY_RIGHT;
        }
        return '\033';
    }

    /*
     * เมื่อใช้ผัง Kedmanee ปุ่มตำแหน่ง A/D ส่ง UTF-8:
     * ฟ (E0 B8 9F) และ ฤ (E0 B8 A4) แทนการขยับซ้าย
     * ก (E0 B8 81) และ ฏ (E0 B8 8F) แทนการขยับขวา
     * อ่านส่วนที่เหลือของชุดอักขระ แล้วแปลงเฉพาะชุดที่รู้จักเป็น KEY_LEFT/RIGHT
     */
    if (key == 0xe0) {
        int second = read_key(10);
        int third = read_key(10);
        if (second == 0xb8) {
            if (third == 0x9f || third == 0xa4) {
                return KEY_LEFT;
            }
            if (third == 0x81 || third == 0x8f) {
                return KEY_RIGHT;
            }
        }
    }
    return key;
}

/*
 * ตรวจตำแหน่งแนวนอนกับทุกช่องของท่อ:
 * ขอบซ้ายรวมอยู่ในช่อง แต่ขอบขวาไม่รวม (ช่วง [ซ้าย, ซ้าย + ความกว้าง))
 * คืน true เมื่อนกอยู่ในช่องใดช่องหนึ่ง มิฉะนั้นคืน false
 */
static bool horizontal_gap_contains(const HorizontalPipe *pipe, int x)
{
    for (int gap = 0; gap < HORIZONTAL_GAP_COUNT; ++gap) {
        if (x >= pipe->gap_left[gap] &&
            x < pipe->gap_left[gap] + HORIZONTAL_GAP_WIDTH) {
            return true;
        }
    }
    return false;
}

/*
 * สร้างและแสดงผลหนึ่งเฟรม
 * พารามิเตอร์ pipes/horizontal_pipes เป็นสถานะอุปสรรคปัจจุบัน
 * bird_x/bird_y ระบุตำแหน่งนก, score เป็นคะแนนปัจจุบัน และ message ใช้แสดงสถานะ
 * ลำดับการวาดสำคัญ: พื้นหลัง -> ท่อ -> นก เพื่อให้นกปรากฏทับองค์ประกอบอื่น
 */
static void draw(const Pipe pipes[PIPE_COUNT], int bird_y, int score,
                 const HorizontalPipe horizontal_pipes[HORIZONTAL_PIPE_COUNT],
                 int bird_x, const char *message)
{
    char field[FIELD_HEIGHT][FIELD_WIDTH + 1];

    /*
     * แต่ละแถวมีช่องข้อมูล FIELD_WIDTH ตัวและช่องเพิ่มหนึ่งตัวสำหรับ '\0'
     * memset เติมเฉพาะส่วนภาพ; การใส่ NUL ทำให้ printf("%s") หยุดตรงท้ายแถว
     */
    for (int y = 0; y < FIELD_HEIGHT; ++y) {
        memset(field[y], '0', FIELD_WIDTH);
        field[y][FIELD_WIDTH] = '\0';
    }

    /*
     * แต่ละท่อกินคอลัมน์ตั้งแต่ x ถึง x + PIPE_WIDTH (ไม่รวมปลายช่วง)
     * คอลัมน์ที่อยู่นอกสนามถูกข้าม ส่วนทุกแถวที่อยู่นอกช่องแนวตั้งจะเป็นกำแพง
     */
    for (int i = 0; i < PIPE_COUNT; ++i) {
        int pipe_left = pipes[i].x;
        for (int x = pipe_left; x < pipe_left + PIPE_WIDTH; ++x) {
            if (x < 0 || x >= FIELD_WIDTH) {
                continue;
            }
            for (int y = 0; y < FIELD_HEIGHT; ++y) {
                if (y < pipes[i].gap_top ||
                    y >= pipes[i].gap_top + GAP_HEIGHT) {
                    field[y][x] = '1';
                }
            }
        }
    }

    /*
     * ท่อแนวนอนกินแถว y ถึง y + HORIZONTAL_PIPE_HEIGHT (ไม่รวมปลายช่วง)
     * สำหรับแต่ละแถว วาดกำแพงทุกคอลัมน์ที่ไม่อยู่ในช่องปลอดภัย
     * การตรวจขอบแถวทำให้ท่อที่ยังอยู่นอกจอไม่เขียนเกินอาร์เรย์ field
     */
    for (int i = 0; i < HORIZONTAL_PIPE_COUNT; ++i) {
        for (int y = horizontal_pipes[i].y;
             y < horizontal_pipes[i].y + HORIZONTAL_PIPE_HEIGHT; ++y) {
            if (y < 0 || y >= FIELD_HEIGHT) {
                continue;
            }
            for (int x = 0; x < FIELD_WIDTH; ++x) {
                if (!horizontal_gap_contains(&horizontal_pipes[i], x)) {
                    field[y][x] = '1';
                }
            }
        }
    }

    /* วาดนกเฉพาะเมื่อพิกัดยังอยู่ภายในสนาม */
    if (bird_y >= 0 && bird_y < FIELD_HEIGHT &&
        bird_x >= 0 && bird_x < FIELD_WIDTH) {
        field[bird_y][bird_x] = '>';
    }

    /*
     * ESC[2J ล้างหน้าจอและ ESC[H ย้ายเคอร์เซอร์มุมซ้ายบน
     * พิมพ์กรอบทีละแถว ตามด้วยคะแนน/คำแนะนำ/ข้อความ และ flush ให้เห็นทันที
     */
    printf("\033[2J\033[H");
    for (int y = 0; y < FIELD_HEIGHT; ++y) {
        printf("|%s|\n", field[y]);
    }
    putchar('+');
    for (int x = 0; x < FIELD_WIDTH; ++x) {
        putchar('-');
    }
    printf("+\n");
    printf("Score: %d    Space: flap    A/D or Left/Right: move    Q: quit\n", score);
    printf("%s\n", message);
    fflush(stdout);
}

/*
 * สุ่มแถวเริ่มต้นของช่องท่อแนวตั้ง
 * บวก 1 และจำกัดค่าสุ่มไว้ก่อนขอบล่างเพื่อให้ช่องไม่ชนกรอบสนาม
 */
static int random_gap_top(void)
{
    return 1 + rand() % (FIELD_HEIGHT - GAP_HEIGHT - 1);
}

/*
 * สุ่มจุดเริ่มต้นของช่องในท่อแนวนอน:
 * latest_first_gap คือขอบสูงสุดที่ยังเหลือพื้นที่สำหรับช่องที่สองและระยะห่าง
 * latest_gap_start คือจุดเริ่มสูงสุดของช่องที่สองก่อนถึงขอบขวา
 * next_gap_min บังคับให้ช่องที่สองอยู่ถัดจากช่องแรกตามระยะขั้นต่ำ
 * rand() % จำนวนตัวเลือก แล้วบวกจุดเริ่มต้น เพื่อสุ่มในช่วงที่ถูกต้อง
 */
static void random_horizontal_gaps(int gap_left[HORIZONTAL_GAP_COUNT])
{
    int latest_first_gap =
        FIELD_WIDTH - 2 * HORIZONTAL_GAP_WIDTH -
        HORIZONTAL_GAP_SEPARATION - 1;
    int latest_gap_start = FIELD_WIDTH - HORIZONTAL_GAP_WIDTH - 1;

    gap_left[0] = 1 + rand() % latest_first_gap;
    int next_gap_min = gap_left[0] + HORIZONTAL_GAP_WIDTH +
                       HORIZONTAL_GAP_SEPARATION;
    gap_left[1] =
        next_gap_min + rand() % (latest_gap_start - next_gap_min + 1);
}

/*
 * เตรียมท่อแนวตั้งเมื่อเริ่มรอบ:
 * วางท่อทั้งหมดนอกขอบขวาและเว้นระยะโดยประมาณเท่า ๆ กัน
 * gap_top สุ่มช่องใหม่ และ scored=false อนุญาตให้ท่อนี้ทำคะแนนในรอบใหม่
 */
static void reset_pipes(Pipe pipes[PIPE_COUNT])
{
    for (int i = 0; i < PIPE_COUNT; ++i) {
        pipes[i].x = FIELD_WIDTH + 5 + i * (FIELD_WIDTH / PIPE_COUNT);
        pipes[i].gap_top = random_gap_top();
        pipes[i].scored = false;
    }
}

/*
 * เตรียมท่อแนวนอนเหนือขอบบน:
 * ค่าลบของ y หมายถึงยังไม่ปรากฏในสนาม และระยะห่างทำให้ท่อทยอยเลื่อนลงมา
 * สุ่มช่องผ่านและล้าง scored เพื่อให้แต่ละท่อให้คะแนนได้หนึ่งครั้ง
 */
static void reset_horizontal_pipes(
    HorizontalPipe pipes[HORIZONTAL_PIPE_COUNT])
{
    for (int i = 0; i < HORIZONTAL_PIPE_COUNT; ++i) {
        pipes[i].y = -HORIZONTAL_PIPE_SPACING * (i + 1);
        random_horizontal_gaps(pipes[i].gap_left);
        pipes[i].scored = false;
    }
}

/*
 * เล่นหนึ่งรอบจนกว่าผู้เล่นจะออกหรือคะแนนจะต่ำกว่าศูนย์
 * คืน true เมื่อแพ้ เพื่อให้ main รอ R/Space/Q; คืน false เมื่อสั่ง Q
 * ตัวแปรคะแนนและตำแหน่งเป็นตัวแปรเฉพาะรอบ จึงเริ่มค่าใหม่ทุกครั้งที่เรียกฟังก์ชัน
 */
static bool play_game()
{
    Pipe pipes[PIPE_COUNT];
    HorizontalPipe horizontal_pipes[HORIZONTAL_PIPE_COUNT];

    /*
     * bird_y/velocity เป็น double เพื่อให้แรงโน้มถ่วงสะสมทีละเศษแถวได้
     * score คือคะแนนปัจจุบันซึ่งลดได้เมื่อชน; max_score เก็บค่าสูงสุดที่เคยทำได้
     * frame และ horizontal_frame เป็นตัวนับแยกกัน เพราะอุปสรรคสองชนิดเคลื่อนที่คนละจังหวะ
     * was_crashed ป้องกันการหักคะแนนซ้ำทุกเฟรมขณะที่ยังชนต่อเนื่อง
     */
    int bird_x = BIRD_X;
    double bird_y = BIRD_Y;
    double velocity = 0.0;
    int score = 0;
    int max_score = 0;
    int frame = 0;
    int horizontal_frame = 0;
    bool was_crashed = false;
    char game_over_message[96];

    reset_pipes(pipes);
    reset_horizontal_pipes(horizontal_pipes);

    /*
     * หนึ่งรอบของลูปคือหนึ่งเฟรมเกม:
     * อ่านอินพุต -> ขยับนก -> อัปเดตอุปสรรค -> ตรวจการชน/คะแนน -> วาดภาพ
     * read_key ใช้ timeout เท่ากับช่วงเฟรม จึงไม่ทำให้เกมหยุดนิ่งเมื่อไม่มีปุ่มกด
     */
    for (;;) {
        int key = read_key(FRAME_MILLISECONDS);

        /*
         * อ่านได้สูงสุดหนึ่งเหตุการณ์ต่อเฟรม:
         * Space กำหนดความเร็วขึ้นใหม่ ส่วนการขยับซ้าย/ขวาเปลี่ยนคอลัมน์ทันที
         * กด Q จากในรอบจะส่ง false กลับไปยัง main เพื่อปิดเกม
         */
        if (key == 'q' || key == 'Q') {
            return false;
        }
        if (key == ' ') {
            velocity = -FLAP_SPEED;
        }
        if (key == KEY_LEFT || key == 'a' || key == 'A') {
            --bird_x;
        }
        if (key == KEY_RIGHT || key == 'd' || key == 'D') {
            ++bird_x;
        }

        /* clamp พิกัด X หลังรับปุ่ม เพื่อไม่ให้นกออกนอกช่วงคอลัมน์ 0..FIELD_WIDTH-1 */
        if (bird_x < 0) {
            bird_x = 0;
        } else if (bird_x >= FIELD_WIDTH) {
            bird_x = FIELD_WIDTH - 1;
        }

        /*
         * ใช้การจำลองแบบง่าย: ความเร็วเพิ่มตามแรงโน้มถ่วง แล้วนำไปบวกตำแหน่ง
         * การกด Space ก่อนหน้านี้ตั้ง velocity เป็นค่าลบ ทำให้ตำแหน่งลดลง (บินขึ้น)
         */
        velocity += GRAVITY;
        bird_y += velocity;

        /*
         * frame นับจำนวนเฟรมตั้งแต่การขยับท่อครั้งก่อน
         * เมื่อครบ PIPE_MOVE_EVERY จึงเลื่อนแต่ละท่อซ้ายหนึ่งคอลัมน์และเริ่มนับใหม่
         * ให้คะแนนหนึ่งแต้มเมื่อขอบขวาท่อผ่าน bird_x; scored ป้องกันแต้มซ้ำ
         * เมื่อท่อออกพ้นขอบซ้าย ให้วางต่อจากท่อที่อยู่ขวาสุดพร้อมช่องสุ่มใหม่
         */
        if (++frame == PIPE_MOVE_EVERY) {
            frame = 0;
            for (int i = 0; i < PIPE_COUNT; ++i) {
                --pipes[i].x;
                if (!pipes[i].scored &&
                    pipes[i].x + PIPE_WIDTH <= bird_x) {
                    pipes[i].scored = true;
                    ++score;
                    /* คะแนนสูงสุดเก็บเฉพาะสถิติที่ทำได้ ไม่ลดตามโทษการชน */
                    if (score > max_score) {
                        max_score = score;
                    }
                }
                if (pipes[i].x + PIPE_WIDTH < 0) {
                    /* หา x ที่มากที่สุด เพื่อวางท่อใบนี้ต่อท้ายแถวโดยไม่ซ้อนกัน */
                    int rightmost = pipes[0].x;
                    for (int j = 1; j < PIPE_COUNT; ++j) {
                        if (pipes[j].x > rightmost) {
                            rightmost = pipes[j].x;
                        }
                    }
                    pipes[i].x = rightmost + FIELD_WIDTH / PIPE_COUNT;
                    pipes[i].gap_top = random_gap_top();
                    pipes[i].scored = false;
                }
            }
        }  

        /*
         * horizontal_frame เป็นตัวนับอิสระจาก frame
         * เมื่อครบ HORIZONTAL_PIPE_MOVE_EVERY เฟรม เลื่อนท่อลงหนึ่งแถว
         * หากท่อพ้นขอบล่าง ให้ย้ายกลับเหนือท่อที่อยู่สูงที่สุดเพื่อรักษาระยะ
         * จากนั้นสุ่มช่องใหม่และล้างสถานะคะแนนสำหรับการผ่านครั้งถัดไป
         */
        if (++horizontal_frame == HORIZONTAL_PIPE_MOVE_EVERY) {
            horizontal_frame = 0;
            for (int i = 0; i < HORIZONTAL_PIPE_COUNT; ++i) {
                ++horizontal_pipes[i].y;
                if (horizontal_pipes[i].y >= FIELD_HEIGHT) {
                    /* หาแถว y ที่น้อยที่สุด (สูงที่สุด) เพื่อจัดท่อใหม่ไว้ด้านบน */
                    int topmost = horizontal_pipes[0].y;
                    for (int j = 1; j < HORIZONTAL_PIPE_COUNT; ++j) {
                        if (horizontal_pipes[j].y < topmost) {
                            topmost = horizontal_pipes[j].y;
                        }
                    }
                    horizontal_pipes[i].y =
                        topmost - HORIZONTAL_PIPE_SPACING;
                    random_horizontal_gaps(horizontal_pipes[i].gap_left);
                    horizontal_pipes[i].scored = false;
                }
            }
        }

        /*
         * สนามเป็นตารางจำนวนเต็ม แต่ bird_y เปลี่ยนแบบเศษส่วน:
         * ปัดเป็นแถวที่ใกล้ที่สุดก่อนใช้วาด/ตรวจชน และชนทันทีหากอยู่นอกกรอบแนวตั้ง
         */
        int bird_row = (int)(bird_y + 0.5);
        bool crashed = bird_row < 0 || bird_row >= FIELD_HEIGHT;

        /*
         * ตรวจเฉพาะเมื่อยังไม่ชน:
         * แนวนอนต้องทับช่วง [pipe_left, pipe_left + PIPE_WIDTH)
         * และแนวตั้งต้องอยู่นอกช่วงช่อง [gap_top, gap_top + GAP_HEIGHT)
         */
        for (int i = 0; i < PIPE_COUNT && !crashed; ++i) {
            int pipe_left = pipes[i].x;
            crashed = bird_x >= pipe_left &&
                      bird_x < pipe_left + PIPE_WIDTH &&
                      (bird_row < pipes[i].gap_top ||
                       bird_row >= pipes[i].gap_top + GAP_HEIGHT);
        }

        /*
         * แถวของนกต้องทับช่วงความสูงของท่อก่อนจึงตรวจตำแหน่ง X
         * ถ้า X อยู่นอกช่องใด ๆ จะชน; ถ้าผ่านและยังไม่เคยได้คะแนนจากท่อนี้
         * เพิ่มคะแนนและตั้ง scored เพื่อไม่ให้แต้มเพิ่มอีกในเฟรมถัดไป
         */
        for (int i = 0; i < HORIZONTAL_PIPE_COUNT && !crashed; ++i) {
            if (bird_row >= horizontal_pipes[i].y &&
                bird_row < horizontal_pipes[i].y + HORIZONTAL_PIPE_HEIGHT) {
                crashed = !horizontal_gap_contains(&horizontal_pipes[i],
                                                   bird_x);
                if (!crashed && !horizontal_pipes[i].scored) {
                    ++score;
                    /* ปรับสถิติสูงสุดหลังได้แต้ม โดยไม่รวมคะแนนที่เสียภายหลัง */
                    if (score > max_score) {
                        max_score = score;
                    }
                    horizontal_pipes[i].scored = true;
                }
            }
        }

        /*
         * เมื่อเริ่มชนชุดใหม่ (was_crashed=false) หักสองคะแนนเพียงครั้งเดียว
         * ถ้ายังชนเฟรมถัดไปจะไม่หักซ้ำ; เมื่อไม่ชนจะล้างธงเพื่อให้ชนครั้งถัดไปถูกนับ
         * ถ้าคะแนนต่ำกว่าศูนย์ สร้างข้อความ Game over พร้อม max_score และจบรอบ
         * หากชนขอบบน/ล่างแต่ยังไม่แพ้ ให้ย้าย bird_y กลับกลางสนามและหยุดความเร็ว
         */
        if (crashed) {
            if (!was_crashed) {
                score -= 2;
                was_crashed = true;
                if (score < 0) {
                    snprintf(
                        game_over_message, sizeof(game_over_message),
                        "Game over! Max score: %d. Press R or Space to play again, Q to quit.",
                        max_score);
                    draw(pipes, bird_row, score, horizontal_pipes, bird_x,
                         game_over_message);
                    return true;
                }
                if (bird_row < 0 || bird_row >= FIELD_HEIGHT) {
                    bird_y = BIRD_Y;
                    velocity = 0.0;
                }
            }
        } else {
            was_crashed = false;
        }

        /* วาดหลังคำนวณสถานะครบ เพื่อให้ภาพที่แสดงตรงกับเฟรมปัจจุบัน */
        draw(pipes, bird_row, score, horizontal_pipes, bird_x, "");
    }
}

/*
 * จุดเริ่มต้นของโปรแกรม:
 * เตรียมการสุ่มและเทอร์มินัล แสดงหน้ารอเริ่ม จากนั้นวนรอเริ่ม/เล่นซ้ำ/ออก
 */
int main()
{
    bool keep_playing = true;

    /*
     * ตั้ง seed จากเวลาปัจจุบัน เพื่อไม่ให้ rand() สร้างลำดับเดิมทุกครั้งที่เปิดเกม
     * srand() ถูกเรียกครั้งเดียวตอนเริ่มโปรแกรม ไม่ใช่ทุกครั้งที่เริ่มรอบ
     */
    srand((unsigned int)time(NULL));

    // ตั้งค่าเทอร์มินัลสำหรับเกมแบบรับปุ่มทันที
    if (!setup_terminal()) {
        return EXIT_FAILURE;
    }

    /*
     * แสดงคำแนะนำเริ่มต้นหนึ่งครั้ง:
     * - ก่อนเริ่มรอบ: Space เข้า play_game()
     * - หลังแพ้: R หรือ Space เรียก play_game() อีกครั้ง; Q ออกจากลูปรอ
     * - เมื่อ play_game() คืน false จะออกจากเกมโดยไม่รอเริ่มรอบใหม่
     */
    printf("\033[2J\033[H");
    printf("FLAPPY BIRD - CLI\n\n");
    printf("Space: flap   A/D or Left/Right: move   Avoid the pipes.\n");
    printf("Press Q at any time to quit.\n");
    fflush(stdout);

    while (keep_playing) {
        int key = read_key(-1);
        if (key == 'q' || key == 'Q') {
            break;
        }
        if (key == ' ') {
            /* การเรียกครั้งแรกจากหน้ารอเริ่มเกม */
            keep_playing = play_game();
            while (keep_playing) {
                key = read_key(-1);
                if (key == 'q' || key == 'Q') {
                    keep_playing = false;
                } else if (key == 'r' || key == 'R' || key == ' ') {
                    /* play_game สร้างสถานะรอบใหม่ทั้งหมดภายในฟังก์ชัน */
                    keep_playing = play_game();
                }
            }
        }
    }

    /* คืนค่าเทอร์มินัลก่อนคืนสถานะออกจากโปรแกรม */
    restore_terminal();
    return EXIT_SUCCESS;
}
