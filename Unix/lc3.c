#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
/* unix */
#include <unistd.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>


//65536个地址
uint16_t memory[UINT16_MAX];

//寄存器
enum 
{ 
    R_R0 = 0, 
    R_R1, 
    R_R2, 
    R_R3, 
    R_R4, 
    R_R5, 
    R_R6, 
    R_R7, 
    R_PC, // 程序计数器
    R_COND, //条件标志寄存器
    R_COUNT 
};

//寄存器存储数组
uint16_t reg[R_COUNT];

//操作码
enum 
{ 
    OP_BR = 0 , //分支
    OP_ADD ,    // 添加  
    OP_LD ,     // 加载  
    OP_ST ,     //存储  
    OP_JSR ,    // 跳转寄存器  
    OP_AND ,    // 按位和 
    OP_LDR ,    // 加载寄存器  
    OP_STR ,    // 存储寄存器  
    OP_RTI ,    // 未使用 
    OP_NOT ,    // 按位不  
    OP_LDI ,    // 加载间接  
    OP_STI ,    // 存储间接
                                                
    OP_JMP ,    // 跳转 
    OP_RES ,    // 保留（未使用） 
    OP_LEA ,    // 加载有效地址 
    OP_TRAP     // 执行陷阱 
};

/* Condition Flags */
enum
{
    FL_POS = 1 << 0, /* P */
    FL_ZRO = 1 << 1, /* Z */
    FL_NEG = 1 << 2, /* N */
};
//内存映射寄存器
enum
{
    MR_KBSR = 0xFE00, //键盘状态
    MR_KBDR = 0xFE02  //键盘数据
};

//陷入例程系统API
enum
{
    TRAP_GETC = 0x20,  // 从键盘获取字符，不回显到终端
    TRAP_OUT = 0x21,   // 输出一个字符
    TRAP_PUTS = 0x22,  // 输出一个字符串
    TRAP_IN = 0x23,    // 获取来自键盘的字符，回显到终端
    TRAP_PUTSP = 0x24, // 输出一个字节串
    TRAP_HALT = 0x25   // 停止程序
};


//填充长度不足16位的命令
uint16_t sign_extend(uint16_t x, int bit_count)
{
    if ((x >> (bit_count - 1)) & 1) 
    {
        x |= (0xFFFF << bit_count);
    }
    return x;
}

//交换大小端
uint16_t swap16(uint16_t x)
{
    return (x << 8) | (x >> 8);
}

//更新标志符号
void update_flags(uint16_t r)
{
    if (reg[r] == 0)
    {
        reg[R_COND] = FL_ZRO;
    }
    else if (reg[r] >> 15) /* a 1 in the left-most bit indicates negative */
    {
        reg[R_COND] = FL_NEG;
    }
    else
    {
        reg[R_COND] = FL_POS;
    }
}

//读取Image File
void read_image_file(FILE* file)
{
    //在内存中放置文件的位置
    uint16_t origin;
    fread(&origin, sizeof(origin), 1, file);
    origin = swap16(origin);

    uint16_t max_read = UINT16_MAX - origin;
    uint16_t* p = memory + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);

    //转换到小端
    while (read-- > 0)
    {
        *p = swap16(*p);
        ++p;
    }
}

//接收路径读取文件
int read_image(const char* image_path)
{
    FILE* file = fopen(image_path, "rb");
    if (!file) 
    { 
        return 0; 
    };
    read_image_file(file);
    fclose(file);
    return 1;
}

//检查键
uint16_t check_key()
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(1, &readfds, NULL, NULL, &timeout) != 0;
}


//写入内存
void mem_write(uint16_t address, uint16_t val)
{
    memory[address] = val;
}

//读取内存
uint16_t mem_read(uint16_t address)
{
    if (address == MR_KBSR)
    {
        if (check_key())
        {
            memory[MR_KBSR] = (1 << 15);
            memory[MR_KBDR] = getchar();
        }
        else
        {
            memory[MR_KBSR] = 0;
        }
    }
    return memory[address];
}

/*--------------------Unix平台细节----------------------------*/
//输入缓冲
struct termios original_tio;

void disable_input_buffering()
{
    tcgetattr(STDIN_FILENO, &original_tio);
    struct termios new_tio = original_tio;
    new_tio.c_lflag &= ~ICANON & ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_input_buffering()
{
    tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}

//处理中断
void handle_interrupt(int signal)
{
    restore_input_buffering();
    printf("\n");
    exit(-2);
}


int main(int argc, const char* argv[])
{
    //加载参数
    if (argc < 2)
    {
        //输出字符串信息
        printf("lc3 [image-file1] ...\n");
        exit(2);
    }
    
    for (int j = 1; j < argc; ++j)
    {
        if (!read_image(argv[j]))
        {
            printf("failed to load image: %s\n", argv[j]);
            exit(1);
        }
    }

    //初始化设置
    signal(SIGINT, handle_interrupt);
    disable_input_buffering();


    //默认将开始位置设为0x3000，为陷入程序代码留出空间
    enum { PC_START = 0x3000 };
    reg[R_PC] = PC_START;

    int running = 1;
    while (running)
    {
        //匹配操作码
        uint16_t instr = mem_read(reg[R_PC]++);
        uint16_t op = instr >> 12;

        switch (op)
        {
            case OP_ADD:
                //AND操作
                {
                    // 目标寄存器 (DR)
                    uint16_t r0 = (instr >> 9) & 0x7;
                    // 第一个操作数
                    uint16_t r1 = (instr >> 6) & 0x7;
                    // 是否处于立即模式,在立即模式下，第二个值嵌入在指令的最右边 5 位中。
                    uint16_t imm_flag = (instr >> 5) & 0x1;
                    //小于 16 位的值需要进行符号扩展
                    if (imm_flag)
                    {
                        uint16_t imm5 = sign_extend(instr & 0x1F, 5);
                        reg[r0] = reg[r1] & imm5;
                    }
                    else
                    {
                        uint16_t r2 = instr & 0x7;
                        reg[r0] = reg[r1] & reg[r2];
                    }
                    update_flags(r0);
                }

                break;
            case OP_AND:
                //按位和
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t r1 = (instr >> 6) & 0x7;
                    uint16_t imm_flag = (instr >> 5) & 0x1;
                
                    if (imm_flag)
                    {
                        uint16_t imm5 = sign_extend(instr & 0x1F, 5);
                        reg[r0] = reg[r1] & imm5;
                    }
                    else
                    {
                        uint16_t r2 = instr & 0x7;
                        reg[r0] = reg[r1] & reg[r2];
                    }
                    update_flags(r0);
                }

                break;
            case OP_NOT:
                //按位否
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t r1 = (instr >> 6) & 0x7;
                
                    reg[r0] = ~reg[r1];
                    update_flags(r0);
                }

                break;
            case OP_BR:
                //分支
                {
                    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                    uint16_t cond_flag = (instr >> 9) & 0x7;
                    if (cond_flag & reg[R_COND])
                    {
                        reg[R_PC] += pc_offset;
                    }
                }

                break;
            case OP_JMP:
                //跳转
                {
                    //r1=7时RET
                    uint16_t r1 = (instr >> 6) & 0x7;
                    reg[R_PC] = reg[r1];
                }

                break;
            case OP_JSR:
                //跳转寄存器
                {
                    uint16_t long_flag = (instr >> 11) & 1;
                    reg[R_R7] = reg[R_PC];
                    if (long_flag)
                    {
                        uint16_t long_pc_offset = sign_extend(instr & 0x7FF, 11);
                        reg[R_PC] += long_pc_offset; 
                    }
                    else
                    {
                        uint16_t r1 = (instr >> 6) & 0x7;
                        reg[R_PC] = reg[r1]; 
                    }
                    break;
                }

                break;
            case OP_LD:
                //加载
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                    reg[r0] = mem_read(reg[R_PC] + pc_offset);
                    update_flags(r0);
                }

                break;
            case OP_LDI:
                // 间接加载指令,用于将内存中某个位置的值加载到寄存器中
                {
                    // 目标寄存器 (DR)
                    uint16_t r0 = (instr >> 9) & 0x7;
                    // 得到偏移量
                    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                    // 将偏移量添加到当前PC，查看该内存位置以获取最终地址
                    reg[r0] = mem_read(mem_read(reg[R_PC] + pc_offset));
                    update_flags(r0);
                }

                break;
            case OP_LDR:
                //加载寄存器
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t r1 = (instr >> 6) & 0x7;
                    uint16_t offset = sign_extend(instr & 0x3F, 6);
                    reg[r0] = mem_read(reg[r1] + offset);
                    update_flags(r0);
                }

                break;
            case OP_LEA:
                //加载有效地址
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                    reg[r0] = reg[R_PC] + pc_offset;
                    update_flags(r0);
                }

                break;
            case OP_ST:
                //存储
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                    mem_write(reg[R_PC] + pc_offset, reg[r0]);
                }

                break;
            case OP_STI:
                //间接存储
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                    mem_write(mem_read(reg[R_PC] + pc_offset), reg[r0]);
                }

                break;
            case OP_STR:
                //存储寄存器
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t r1 = (instr >> 6) & 0x7;
                    uint16_t offset = sign_extend(instr & 0x3F, 6);
                    mem_write(reg[r1] + offset, reg[r0]);
                }

                break;
            case OP_TRAP:
                //处理陷入，调用系统API
                switch (instr & 0xFF)
                {
                    case TRAP_GETC:
                        //读取单个 ASCII 字符
                        reg[R_R0] = (uint16_t)getchar();

                        break;
                    case TRAP_OUT:
                        /* TRAP OUT */
                        putc((char)reg[R_R0], stdout);
                        fflush(stdout);

                        break;
                    case TRAP_PUTS:
                        //实现puts，将一串 ASCII 字符写入控制台显示。
                        //字符包含在连续的内存位置中，从R0指定的位置开始。
                        {
                            //一个字符两字节
                            uint16_t* c = memory + reg[R_R0];
                            while (*c)
                            {
                                putc((char)*c, stdout);
                                ++c;
                            }
                            fflush(stdout);
                        }

                        break;
                    case TRAP_IN:
                        //输入字符
                        {
                            printf("请输入字符: ");
                            char c = getchar();
                            putc(c, stdout);
                            reg[R_R0] = (uint16_t)c;
                        }

                        break;
                    case TRAP_PUTSP:
                        // 输出字符串
                        {
                            // 转换回大端格式
                            uint16_t* c = memory + reg[R_R0];
                            while (*c)
                            {
                                char char1 = (*c) & 0xFF;
                                putc(char1, stdout);
                                char char2 = (*c) >> 8;
                                if (char2) putc(char2, stdout);
                                ++c;
                            }
                            fflush(stdout);
                        }

                        break;
                    case TRAP_HALT:
                        //暂停
                        puts("HALT");
                        fflush(stdout);
                        running = 0;

                        break;
                }

                break;
            case OP_RES:
            case OP_RTI:
            default:
                // 终止
                abort();
                break;
        }
    }
    /* Shutdown */
    restore_input_buffering();

}


