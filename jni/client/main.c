#include "osapi.h"
#include "list.h"

#include "iarch_mgmt.h"
#include "iarch_stor.h"
#include "fixbuffer.h"
#include "fixarray.h"

#include "cli-mgmt.h"
#include "cli-stor.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#define cli_ADDR   "127.0.0.1"
#define cli_PORT   8086

static client_t cli;
static session_t ses;

int cli_create_remote(struct sockaddr *addr, int len){
    int rc;

    rc = session_init(&ses);
    if(rc != 0){
        return rc;
    }
    
    rc = session_connect(&ses, addr, len);
    if(rc != 0){
        return rc;
    }

    client_init(&cli, &ses);

    return 0;
}

int cli_destroy(client_t *cli){
    assert(cli != NULL);

    session_cleanup(cli->ses);
    session_fini(cli->ses);

    client_fini(cli);

    return 0;
}


static int setup_cli(){
    struct sockaddr_in  sin;
    int rc;
    
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(cli_ADDR);
    sin.sin_port = htons(cli_PORT);

    rc = cli_create_remote((struct sockaddr*)&sin, sizeof(sin));
    if(rc != 0){
        printf("init remote cli fail:%d\n", rc);
        return -1;
    }

    return 0;
}

static void usage(){
    printf("usage:  program command param ...\n");
    printf("command:\n");
    printf("    list {video audio photo docs} [folder-id]\n");
    printf("    push local-filename {video audio photo docs}[/remote-filename]\n");
    printf("    pull file-id local-filename\n");
    printf("    move file-id folder-id [new-name]\n");
    printf("    rm   file-id\n");
}

static const char kftypes[][6] = {
    "video",
    "audio",
    "photo",
    "docs",
};

static int get_type(const char *type){
    int i;

    for(i = 0; i < 4; i++){
        if(strcmp(type, kftypes[i]) == 0){
            return i;
        }
    }
    return -1;
}

static int list_func(client_t *cli, const char *type, const char *folderid, const char *notused){
    int folder = 0;
    int ftype = -1;
    int i;
    int64_t luid;
    fixarray_t *fa;
    rfile_t *rf;
    int num = 20;
    int rc = 0;

    ftype = get_type(type);

    if(ftype < 0){
        usage();
        return -1;
    }

    if(folderid != NULL){
        folder = atoi(folderid);
    }

    luid = clis_lookup_create(cli, folder, ftype, 1);
    if(luid < 0){
        printf("create lookup fail!\n");
        return -1;
    }
    printf("create lookup ok\n");

    while(1){
        rc = clis_lookup_next(cli, luid, num, &fa);
        if(rc != 0){
            printf("lookup fetch data fail!\n");
            break;
        }

        for(i = 0; i < fixarray_num(fa); i++){
            rc = fixarray_get(fa, i, (void **)&rf);
            if(rc != 0)
                break;

            printf("name:%s folder:%s - id:%"PRId64" - len:%"PRId64"\n",
                    rf->fname, (rf->folder == NULL) ? "/":rf->folder, rf->fid, rf->fsize);
            
            stor_fstat_rsp_free((stor_fstat_rsp_t *)rf);
        }

        if(fixarray_num(fa) != num){
            fixarray_destroy(fa);
            break;
        }
        fixarray_destroy(fa);
        fa = NULL;
    }

    clis_lookup_destroy(cli, luid);
    
    return 0;
}

static int push_func(client_t *cli, const char *lofname, const char *refname, const char *notused){
    char *rtype;
    char *rfile;
    char *temp;
    int type;
    int64_t fid;
    FILE *handle;
    int rc;
    int i;
    void *buf;
    size_t len = 4096, rlen;
    off_t offset = 0;

    // prepare params
    if(refname == NULL){
        usage();
        return -1;
    }
    rtype = strdup(refname);
    rfile = strchr(rtype, '/');
    if(rfile == NULL){
        temp = strrchr(lofname, '/');
        if(temp == NULL){
            rfile = (char *)lofname;
        }else{
            rfile = temp + 1;
        }
    }else{
        rfile[0] = 0;
        rfile += 1;
    }

    type = get_type(rtype);
    if(type < 0){
        free(rtype);
        usage();
        return -1;
    }

    // check local file is exist
    handle = fopen(lofname, "r+b");
    if(handle == NULL){
        printf("local file :%s can't be open!\n", lofname);
        free(rtype);
        return -1;
    }

    // create file
    fid = clis_file_create(cli, rfile, type, 0);
    if(fid < 0){
        printf("create remote file:%s fail\n", rfile);
        free(rtype);
        fclose(handle);
        return -1;
    }
    free(rtype);

    rc = clis_file_open(cli, fid);
    if(rc != 0){
        printf("open remote file fail\n");
        return -1;
    }

    buf = malloc(len);
    if(buf == NULL){
        printf("alloc memory fail\n");
        return -1;
    }

    i = 0;
    while(1){
        rlen = fread(buf, 1, len, handle);
        if(rlen > 0){
            rc = clis_file_write(cli, fid, offset, buf, rlen);
            if(rc != rlen){
                printf("write remote file fail\n");
                break;
            }
            offset += rlen;
        }else{
            break;
        }
        
        if((i++%100) == 0)
            printf(".");
    }

    clis_file_close(cli, fid);
    fclose(handle);

    printf("\npush file ok\n");

    return 0;
}

static int pull_func(client_t *cli, const char *fidstr, const char *lofname, const char *notused){
    FILE *handle;
    int64_t fid;
    void *buf;
    size_t len = 4096, wlen;
    int i, rc;

    fid = atoi(fidstr);
    if(lofname == NULL){
        printf("you must give local file name to receive data!\n");
        return -1;
    }
    
    handle = fopen(lofname, "wb+");
    if(handle == NULL){
        printf("open local file for write fail!\n");
        return -1;
    }
    
    rc = clis_file_open(cli, fid);
    if(rc != 0){
        printf("open remote file fail\n");
        fclose(handle);
        return -1;
    }

    i = 0;
    buf = malloc(len);
    if(buf == NULL){
        printf("no memory for pull\n");
        return -1;
    }

    while(1){
        rc = clis_file_read(cli, fid, i*len, buf, len);
        if(rc < 0){
            printf("read remote file fail\n");
            fclose(handle);
            free(buf);
            return -1;
        }

        wlen = fwrite(buf, 1, (size_t)rc, handle);
        if(wlen != (size_t)rc){
            printf("write localfile fail\n");
            return -1;
        }
        i++;

        if((size_t)rc != wlen){
            printf("not all data have been write to local\n");
            break;
        }

        if((i%100) == 0)
            printf(".");

        if(rc != len)
            break;
    }

    fclose(handle);
    clis_file_close(cli, fid);
    
    printf("\npull file ok\n");

    return 0;
}

static int move_func(client_t *cli, const char *fidstr, const char *folderidstr,
        const char *new_name){
    int64_t fid;
    int64_t folder;
    char *nname = NULL;

    fid = atoi(fidstr);
    folder = atoi(folderidstr);
    if(new_name != NULL){
        nname = strdup(new_name);
    }

    if(clis_file_moveto(cli, fid, folder, nname) != 0){
        printf("move file fail!\n");
        return -1;
    }

    printf("move file ok\n");

    return 0;
}

static int rm_func(client_t *cli, const char *fidstr, const char *notused1,
        const char *notused2){
    int64_t fid;

    fid = atoi(fidstr);

    if(clis_file_delete(cli, fid) != 0){
        printf("rm file fail!\n");
        return -1;
    }

    printf("rm file ok\n");

    return 0;
}

typedef int (*cmd_func)(client_t *cli, const char *p0, const char *p1, const char *p2); 
struct _cmd{
    char *cmd;
    cmd_func  func;
};

struct _cmd  _cmds[] = {
    {"list", list_func},
    {"push", push_func},
    {"pull", pull_func},
    {"move", move_func},
    {"rm",   rm_func}
};

int main(int argc, char *argv[]){
    char *cmd;
    char *p0=NULL, *p1=NULL, *p2=NULL;
    int  i;
    struct _cmd *c;

    if(argc < 2){
        usage();
        return -1;
    }

    cmd = argv[1];
    switch(argc){
        case 4: 
            p2 = argv[4];

        case 3:
            p1 = argv[3];

        case 2:
            p0 = argv[2];
            break;

        default :
            usage();
            return -1;
    }

    if(setup_cli() != 0){
        return -1;
    }

    for(i = 0; i < sizeof(_cmds)/sizeof(struct _cmd); i++){
        c = &(_cmds[i]);
        if(strcmp(c->cmd, cmd) == 0){
            return c->func(&cli, p0, p1, p2);
        }
    }

    usage();
    return -1;
}


