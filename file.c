#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/times.h>
#include <time.h>

// 块的划分：
// 0        超级块
// 1        组描述符
// 2        块位图
// 3        inode位图
// 4~35     inode table
// 36~32767 数据

typedef unsigned short us;
clock_t start,end;
struct tms t_start,t_end;
char cmd[200],cmd_s[3][120],full_path[400];
us now_dir;//记录当前所在目录的inode

//块组：
union block{
    //超级块×1
    struct {
        us s_inodes_count;      //索引节点的总数
        us s_data_blocks_count; //数据块的总数
        us s_blocks_count;      //以块为单位的文件系统的大小
    } super_block;
    //组描述符×1
    struct {
        us bg_block_bitmap;     //块位图的块号
        us bg_inode_bitmap;     //索引节点位图的块号
        us bg_inode_table;      //第一个索引节点表块的块号
        us bg_free_blocks_count;//组中空闲块的个数
        us bg_free_inodes_count;//组中空闲索引节点的个数
        us bg_used_dirs_count;  //组中目录的个数
    } group_desc;
    //块位图×1
    unsigned char block_bit_map[0x1000]; //从低位到高位存储, 0: 空闲, 1:占用
    //inode位图×1
    unsigned char inode_bit_map[0x1000]; //从低位到高位存储, 0: 空闲, 1:占用
    //inode table×32
    struct {
        us type;            //文件类型，0:未知 1:普通文件 2:目录
        us mode;            //文件权限，0:未知 4:r 2:w 1:x
        unsigned int size;  //文件大小，字节为单位
        float mtime;        //文件最后修改时间，即从程序运行开始的秒数
        us block_count;     //文件的数据块数
        us block_pointer;   //首个数据块的地址
        char name[100];     //文件名,112
    } inode_table[32];
    //数据块×n
    us data[0x800];
} b[0x8000];

//功能函数：
//记录时间
float time_calc(clock_t time){
	long tps = sysconf(_SC_CLK_TCK);//获取系统每秒钟的滴答数
	return (float)time/tps;//将嘀嗒数转化成秒
}
//分割命令
int scan_cmd(){
    scanf("%[^\n]",cmd);
    getchar();//接收回车，否则会出错
    memset(cmd_s,0,sizeof(cmd_s));
    int n = 0;
    char *result = NULL;
    result = strtok(cmd, " ");//以空格为分割符
    while( result != NULL )
    {
        strcpy(cmd_s[n++], result);
        result = strtok(NULL, " ");
    }
    return n;
}

//文件系统功能函数：
//通过inode获取文件名
char *inode_to_name(us inode){
    return b[4+(inode/32)].inode_table[inode%32].name;
}
//查询数据块位，使用实际块号，范围：36~32767
us block_bit_get(int pos){
    char get = 1;
    get <<= (pos%8);
    return b[2].block_bit_map[pos/8] & get;
}
//启用数据块位，使用实际块号，范围：36~32767
void block_bit_set(int pos){
    char set = 1;
    set <<= (pos%8);
    b[2].block_bit_map[pos/8] |= set;
}
//清除数据块位，使用实际块号，范围：36~32767
void block_bit_clear(int pos){
    char set = 1;
    set <<= (pos%8);
    b[2].block_bit_map[pos/8] &= ~set;
}
//查询inode是否使用，范围：0~1023
us inode_bit_get(int pos){
    char get = 1;
    get <<= (pos%8);
    return b[3].inode_bit_map[pos/8] & get;
}
//启用inode位，范围：0~1023
void inode_bit_set(int pos){
    char set = 1;
    set <<= (pos%8);
    b[3].inode_bit_map[pos/8] |= set;
}
//清除inode位，范围：0~1023
void inode_bit_clear(int pos){
    char set = 1;
    set <<= (pos%8);
    b[3].inode_bit_map[pos/8] &= ~set;
}
//获取空闲数据块，返回实际块号
us get_free_block_num(){
    for(us  i=0;i<0x1000;i++){
        if(b[2].block_bit_map[i]!=0xff){
            unsigned char temp = b[2].block_bit_map[i];
            //找到第一个为0的位
            if(!temp)
                return i*8+0;
            us pos = 1;
            temp = ~temp;
            while((temp>>pos<<pos) ==  temp){
                pos++;
            }
            return i*8+pos-1;
        }
    }
    return 0;
}
//获取空闲inode
int get_free_inode_num(){
    for(int i=0;i<0x1000;i++){
        if(b[3].inode_bit_map[i]!=0xff){
            unsigned char temp = b[3].inode_bit_map[i];
            //找到第一个为0的位
            if(!temp)
                return i*8+0;
            int pos = 1;
            temp = ~temp;
            while((temp>>pos<<pos) ==  temp){
                pos++;
            }
            return i*8+pos-1;
        }
    }
    return -1;
}
//检查重名 inode:有重名 0:无重名
us check_filename(char *name){
    us ablock = b[4+(now_dir/32)].inode_table[now_dir%32].block_pointer;//数据块
    while(ablock){
        //略过块指针、.目录、..目录
        for(int i=3;i<0x800;i++){
            us inode = b[ablock].data[i];
            if(inode){
                if(!strcmp(inode_to_name(inode),name)){
                    return inode;
                }
            }
        }
        //如果有后继数据块，继续检查
        ablock = b[ablock].data[0];
    }
    return 0;
}
//检查目录中的文件个数，判断是否为空目录
us check_file_contain(us delete_dir){
    us count = 0;
    us ablock = b[4+(delete_dir/32)].inode_table[delete_dir%32].block_pointer;//数据块
    while(ablock){
        //略过块指针、.目录、..目录
        for(int i=3;i<0x800;i++){
            us inode = b[ablock].data[i];
            if(inode){
                count++;
            }
        }
        //如果有后继数据块，继续检查
        ablock = b[ablock].data[0];
    }
    return count;
}
//设置inode的各个属性
void set_inode(us inode,us type,us mode,unsigned int size,us block_count,us block_pointer,char *name){
    b[4+(inode/32)].inode_table[inode%32].type = type;
    b[4+(inode/32)].inode_table[inode%32].mode = mode;
    b[4+(inode/32)].inode_table[inode%32].size = size;
    b[4+(inode/32)].inode_table[inode%32].mtime = time_calc(times(&t_end) - start);
    b[4+(inode/32)].inode_table[inode%32].block_count = block_count;
    b[4+(inode/32)].inode_table[inode%32].block_pointer = block_pointer;
    strcpy(b[4+(inode/32)].inode_table[inode%32].name,name);
}
//读取inode的详细属性
void get_inode(us inode){
    us mode = b[4+(inode/32)].inode_table[inode%32].mode;
    if(mode/4) 
        putchar('r');
    else
        putchar('-');
    mode %= 4;
    if(mode/2) 
        putchar('w');
    else
        putchar('-');
    mode %= 2;
    if(mode) 
        putchar('x');
    else
        putchar('-');
    if(b[4+(inode/32)].inode_table[inode%32].type == 2)
        printf(" DIR  ");
    else
        printf(" FILE ");
    int sec = (int)b[4+(inode/32)].inode_table[inode%32].mtime;
    printf("%7d  %d:%05.2f\t%s\n",b[4+(inode/32)].inode_table[inode%32].size,
            sec/60,b[4+(inode/32)].inode_table[inode%32].mtime - sec + (sec%60) ,
            b[4+(inode/32)].inode_table[inode%32].name);
}
//清理一个数据块。清理工作在该块被重用时进行，而非删除时
void clear_block(us num){
    for(int i=0;i<0x800;i++)
        b[num].data[i] = 0;
}
//16进制快捷转换为二进制
char *HEX[16] = {"0000","0001","0010","0011",
                "0100","0101","0110","0111",
                "1000","1001","1010","1011",
                "1100","1101","1110","1111",
                };
//查看所有数据块的占用情况（便于调试，此命令隐藏）
void dis_data_block(){
    for(int i=0;i<0x1000;i++){
        if(b[2].block_bit_map[i]){
            printf("%d\t~%d:\t%s%s\n",8*i,8*i+7,HEX[b[2].block_bit_map[i]/16],HEX[b[2].block_bit_map[i]%16]);
        }
    }
}

//命令函数：
//初始化
void init(){
    memset(b,0,sizeof(b));
    //初始化超级块
    b[0].super_block.s_inodes_count = 1024;
    b[0].super_block.s_data_blocks_count = 32732;
    b[0].super_block.s_blocks_count = 32768;
    //初始化组描述符
    b[1].group_desc.bg_block_bitmap = 2;
    b[1].group_desc.bg_inode_bitmap = 3;
    b[1].group_desc.bg_inode_table = 4;
    b[1].group_desc.bg_free_blocks_count = 32732;
    b[1].group_desc.bg_free_inodes_count = 1024;
    b[1].group_desc.bg_used_dirs_count = 0;
    //初始化块位图，前36块已被使用
    b[2].block_bit_map[0] = 0xff;
    b[2].block_bit_map[1] = 0xff;
    b[2].block_bit_map[2] = 0xff;
    b[2].block_bit_map[3] = 0xff;
    b[2].block_bit_map[4] = 0xf0;
    //建立root根目录
    b[1].group_desc.bg_free_blocks_count--;
    b[1].group_desc.bg_free_inodes_count--;
    b[1].group_desc.bg_used_dirs_count++;
    block_bit_set(36);
    inode_bit_set(0);
    b[4].inode_table[0].type = 2;
    b[4].inode_table[0].mode = 6;
    b[4].inode_table[0].size = 0;
    b[4].inode_table[0].mtime = time_calc(start - start);
    b[4].inode_table[0].block_pointer = 36;
    b[4].inode_table[0].block_count = 1;
    strcpy(b[4].inode_table[0].name,"root");
    b[36].data[1] = 0;// .目录的inode指向自己
    b[36].data[2] = 0;// root没有..目录，指向自己
}
//进入目录
void cd(char *name){
    //只能进入子目录、本目录、父目录，不能指定绝对路径。可以使用cd /或cd root回到根目录
    us ablock = b[4+(now_dir/32)].inode_table[now_dir%32].block_pointer;//数据块
    if(!strcmp(name,"/") || !strcmp(name,"root")){
        now_dir = 0;
    }else if(!strcmp(name,"..")){
        now_dir = b[ablock].data[2];
    }else if(strcmp(name,".")){
        int find = check_filename(name);
        if(find)
            now_dir = find;
        else
            printf("错误：未找到目录！\n");
    }
}
//创建目录
void mkdir(char *name){
    //需要额外创建.和..目录
    //找目录数据块中的空位置，插入新目录的inode
    if(!strlen(name)){
        printf("错误：缺少目录名，无法创建！\n");
        return;
    }
    if(!check_filename(name)){
        //检查索引节点和存储空间是否有空闲
        if(b[1].group_desc.bg_free_inodes_count<=0){
            printf("错误：索引节点已满，无法创建！\n");
            return;
        }
        if(b[1].group_desc.bg_free_blocks_count<=0){
            printf("错误：存储空间已满，无法创建！\n");
            return;
        }
        //有空闲，可以创建
        b[1].group_desc.bg_free_inodes_count--;
        b[1].group_desc.bg_free_blocks_count--;
        b[1].group_desc.bg_used_dirs_count++;
        us ablock = b[4+(now_dir/32)].inode_table[now_dir%32].block_pointer;//数据块
        while(ablock){
            //略过块指针、.目录、..目录
            for(int i=3;i<0x800;i++){
                us inode = b[ablock].data[i];
                if(!inode){
                    inode = get_free_inode_num();//获取空inode
                    us first_block = get_free_block_num();//获取空数据块
                    clear_block(first_block);//清理这个数据块里的内容
                    b[ablock].data[i] = inode;//将新目录的inode填到当前目录
                    inode_bit_set(inode);//将inode位图设为1
                    block_bit_set(first_block);//将数据块位图设为1
                    //设置inode的各个属性，目录默认权限为rw
                    set_inode(inode,2,6,4096,1,first_block,name);
                    //设置block（初始化数据）
                    b[first_block].data[0] = 0;
                    b[first_block].data[1] = inode;
                    b[first_block].data[2] = now_dir;
                    printf("信息：新建目录成功！\n");
                    return;
                }
            }
            //如果有后继数据块，继续检查
            ablock = b[ablock].data[0];
        }
    }else{
        printf("错误：重复名称！\n");
    }
}
//删除空目录
void rm_dir(char *name){
    us ablock = b[4+(now_dir/32)].inode_table[now_dir%32].block_pointer;//数据块
    while(ablock){
        //略过块指针、.目录、..目录
        for(int i=3;i<0x800;i++){
            us inode = b[ablock].data[i];
            if(inode){
                if(!strcmp(inode_to_name(inode),name)){
                    //检查是否是空目录
                    if(check_file_contain(inode)){
                        printf("错误：目录非空，不能删除！\n");
                        return;
                    }
                    //恢复inode位图
                    inode_bit_clear(inode);
                    //修改当前工作目录的数据块，删除记录这个目录的信息
                    b[ablock].data[i] = 0;
                    //恢复所有数据块位图
                    us pre_block = b[4+(inode/32)].inode_table[inode%32].block_pointer;//数据块
                    us next_block = 0;
                    do{
                        next_block = b[pre_block].data[0];
                        block_bit_clear(pre_block);
                        pre_block = next_block;
                    }while(next_block);
                    printf("信息：删除成功！\n");
                    return;
                }
            }
        }
        //如果有后继数据块，继续检查
        ablock = b[ablock].data[0];
    }
    printf("错误：未找到目录！\n");
}
//创建新文件，不分配数据块 or 修改文件时间
void touch(char *name){
    us s_inode;
    if(!strlen(name)){
        printf("错误：缺少文件名，无法创建！\n");
        return;
    }
    if(s_inode = check_filename(name)){
        //有重名，修改原文件的时间戳：
        if(b[4+(s_inode/32)].inode_table[s_inode%32].type != 1){
            printf("错误：有重名目录，创建失败！\n");
            return;
        }
        b[4+(s_inode/32)].inode_table[s_inode%32].mtime = time_calc(times(&t_end) - start);//更新时间
        printf("信息：有重名文件，更新源文件时间！\n");
    }else{
        //无重名，建立inode：
        //检查索引节点和存储空间是否有空闲
        if(b[1].group_desc.bg_free_inodes_count<=0){
            printf("错误：索引节点已满，无法创建！\n");
            return;
        }
        if(b[1].group_desc.bg_free_blocks_count<=0){
            printf("错误：存储空间已满，无法创建！\n");
            return;
        }
        //有空闲，可以创建
        b[1].group_desc.bg_free_inodes_count--;
        b[1].group_desc.bg_free_blocks_count--;
        //找目录数据块中的空位置，插入新文件的inode
        us ablock = b[4+(now_dir/32)].inode_table[now_dir%32].block_pointer;//数据块
        while(ablock){
            //略过块指针、.目录、..目录
            for(int i=3;i<0x800;i++){
                us inode = b[ablock].data[i];
                if(!inode){
                    inode = get_free_inode_num();//获取空inode
                    b[ablock].data[i] = inode;//将新目录的inode填到当前目录
                    inode_bit_set(inode);//将inode位图设为1
                    //设置inode的各个属性，文件默认权限为rwx
                    set_inode(inode,1,7,0,0,0,name);
                    printf("信息：新建文件成功！\n");
                    return;
                }
            }
            //如果有后继数据块，继续检查
            ablock = b[ablock].data[0];
        }
    }
}
//创建新文件，分配数据块 or 修改文件大小和时间（以字节为单位）
void touch_size(char *name,unsigned int size){
    us s_inode;
    if(s_inode = check_filename(name)){
        //有重名，修改原文件的时间戳和大小：
        if(b[4+(s_inode/32)].inode_table[s_inode%32].type != 1){
            printf("错误：有重名目录，创建失败！\n");
            return;
        }
        b[4+(s_inode/32)].inode_table[s_inode%32].mtime = time_calc(times(&t_end) - start);//更新时间
        b[4+(s_inode/32)].inode_table[s_inode%32].size = size;//更新大小
        printf("信息：有重名文件，更新源文件的时间和大小！\n");
    }else{
        //无重名，建立inode，分配数据块：
        //计算的所需数据块的数量：
        int blocks = size/4094 + 1;
        //检查索引节点和存储空间是否有空闲
        if(b[1].group_desc.bg_free_inodes_count<=0){
            printf("错误：索引节点已满，无法创建！\n");
            return;
        }
        if(b[1].group_desc.bg_free_blocks_count<=blocks){
            printf("错误：存储空间已满，无法创建！\n");
            return;
        }
        //有空闲，可以创建
        b[1].group_desc.bg_free_inodes_count--;
        b[1].group_desc.bg_free_blocks_count-=blocks;
        //找目录数据块中的空位置，插入新文件的inode
        us ablock = b[4+(now_dir/32)].inode_table[now_dir%32].block_pointer;//数据块
        while(ablock){
            //略过块指针、.目录、..目录
            for(int i=3;i<0x800;i++){
                us inode = b[ablock].data[i];
                if(!inode){
                    inode = get_free_inode_num();//获取空inode
                    us pre_block = get_free_block_num();//获取空数据块
                    block_bit_set(pre_block);//将数据块位图设为1
                    us new_block = 0;
                    us first_block = pre_block;
                    clear_block(pre_block);//清理这个数据块里的内容
                    while(--blocks){
                        new_block = get_free_block_num();//获取空数据块
                        block_bit_set(new_block);//将数据块位图设为1
                        clear_block(new_block);//清理这个数据块里的内容
                        b[pre_block].data[0] = new_block;//设置前一块的指针为后一块
                        pre_block = new_block;
                    }
                    b[ablock].data[i] = inode;//将新目录的inode填到当前目录
                    inode_bit_set(inode);//将inode位图设为1
                    //设置inode的各个属性，文件默认权限为rwx
                    set_inode(inode,1,7,size,size/4094 + 1,first_block,name);
                    printf("信息：新建文件成功！\n");
                    return;
                }
            }
            //如果有后继数据块，继续检查
            ablock = b[ablock].data[0];
        }
    }
}
//删除文件
void rm(char *name){
    us ablock = b[4+(now_dir/32)].inode_table[now_dir%32].block_pointer;//数据块
    while(ablock){
        //略过块指针、.目录、..目录
        for(int i=3;i<0x800;i++){
            us inode = b[ablock].data[i];
            if(inode){
                if(!strcmp(inode_to_name(inode),name)){
                    //恢复inode位图
                    inode_bit_clear(inode);
                    //修改当前工作目录的数据块，删除记录这个文件的信息
                    b[ablock].data[i] = 0;
                    //恢复所有数据块位图
                    us pre_block = b[4+(inode/32)].inode_table[inode%32].block_pointer;//数据块
                    us next_block = 0;
                    do{
                        next_block = b[pre_block].data[0];//通过指针找到后继数据块
                        block_bit_clear(pre_block);//将数据块位图设为0
                        pre_block = next_block;//变换前驱与后继的角色，继续寻找后继
                    }while(next_block);
                    printf("信息：删除成功！\n");
                    return;
                }
            }
        }
        //如果有后继数据块，继续检查
        ablock = b[ablock].data[0];
    }
    printf("错误：未找到文件！\n");
}
//列出目录内容
void ls(){
    if(!strcmp(cmd_s[1],"-a")){
        printf(".\t..\t");//显示隐藏的.和..
    }
    us ablock = b[4+(now_dir/32)].inode_table[now_dir%32].block_pointer;//数据块
    while(ablock){
        //略过块指针、.目录、..目录
        for(int i=3;i<0x800;i++){
            us inode = b[ablock].data[i];
            if(inode && inode_bit_get(inode)){
                printf("%s\t",inode_to_name(inode));
            }
        }
        //如果有后继数据块，继续检查
        ablock = b[ablock].data[0];
    }
    puts("");
}
//列出目录详细内容
void ll(){
    us ablock = b[4+(now_dir/32)].inode_table[now_dir%32].block_pointer;//数据块
    while(ablock){
        //略过块指针、.目录、..目录
        for(int i=3;i<0x800;i++){
            us inode = b[ablock].data[i];
            if(inode && inode_bit_get(inode)){
                get_inode(inode);
            }
        }
        //如果有后继数据块，继续检查
        ablock = b[ablock].data[0];
    }
}
//递归获取父级inode，拼接路径
void get_super_inode(us inode){
    int super_inode = inode;
    if(inode){
        get_super_inode(b[b[4+(inode/32)].inode_table[inode%32].block_pointer].data[2]);//进入递归
        //在递归尾拼接字符串
        strcat(full_path,"/");
        strcat(full_path,inode_to_name(inode));
    }else{
        strcpy(full_path,"/root");
    }
}
//给出当前完整目录
void get_full_path(){
    get_super_inode(now_dir);//递归获取完整路径
    printf("%s$ ",full_path);//每行前面打印的路径提示符
}


int main(){
    //记录程序开始时的时间
	start = times(&t_start);
    //初始化
    init();
    printf("------ 虚拟文件系统 2022/6/29 ------\n");
    printf("初始化完成 [ 进入系统:回车, 退出:q ]");
    char op;
    while(op = getchar()){
        if(op=='\n'){
            while(1){
                memset(cmd,0,sizeof(cmd));//清空命令字符串
                get_full_path();//显示路径提示
                scan_cmd();//读取用户输入
                if(!strcmp(cmd_s[0],"touch")){
                    if(strlen(cmd_s[2]))
                        touch_size(cmd_s[2],atoi(cmd_s[1]));
                    else
                        touch(cmd_s[1]);
                }else if(!strcmp(cmd_s[0],"rm")){
                    rm(cmd_s[1]);
                }else if(!strcmp(cmd_s[0],"ls")){
                    if(strcmp(cmd_s[1],"-l"))
                        ls();
                    else
                        ll();
                }else if(!strcmp(cmd_s[0],"mkdir")){
                    mkdir(cmd_s[1]);
                }else if(!strcmp(cmd_s[0],"rmdir")){
                    rm_dir(cmd_s[1]);
                }else if(!strcmp(cmd_s[0],"cd")){
                    cd(cmd_s[1]);
                }else if(!strcmp(cmd_s[0],"help")){
                    puts("帮助：\n\tls\t列出目录内容\n"
                        "\t|\t-a\t列出隐藏文件\n"
                        "\t|\t-l\t列出详细信息\n"
                        "\tcd\t改变工作目录\n"
                        "\tmkdir\t创建目录\n"
                        "\trmdir\t删除目录\n"
                        "\ttouch\t创建文件\n"
                        "\t|\tfilename\t普通创建\n"
                        "\t|\tsize filename\t指定大小\n"
                        "\trm\t删除文件");
                }else if(!strcmp(cmd_s[0],"exit")){
                    printf("已退出系统 [ 进入系统:回车, 退出:q ]\n");
                    break;
                }else if(!strcmp(cmd_s[0],"dis")){
                    dis_data_block();//调试用
                }else if(strlen(cmd_s[0])){
                    printf("信息：不支持的命令\n");
                }
            }
        }else if(op=='q'){
            break;
        }
    }
    printf("退出\n");
}