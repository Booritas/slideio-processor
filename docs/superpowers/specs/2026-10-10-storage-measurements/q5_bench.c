/* THROWAWAY SPIKE - measurement 1: physical key scheme for the objects table.
   Derived from q4_bench.c (2026-10-07) and deliberately kept comparable:
   same slide, same tiling, same RNG seed, same batch size, same pragmas.

   Three modes, all producing the SAME anchor sequence:
     0  anchor      - anchor IS the INTEGER PRIMARY KEY          (today's SDS 03)
     1  anchor-sort - same schema, each batch sorted by anchor   (cheap partial fix)
     2  rowid-index - monotonic rowid PK + anchor column
                      + UNIQUE INDEX(layer,anchor)               (the SDS 03 proposal)

   Mode 2 is NOT the old "sequential" mode: that one dropped the anchor
   entirely and so could not enforce the identity UR-QA-3 requires. The cost of
   the unique index on a random 64-bit key is the thing being measured. */
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define SLIDE_W   100000
#define SLIDE_H    80000
#define TILE       1024
#define GEOM_BYTES  200

static double now_s(void){
    LARGE_INTEGER f,t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return (double)t.QuadPart/(double)f.QuadPart;
}
static void must(int rc, sqlite3*db, const char*what){
    if(rc!=SQLITE_OK && rc!=SQLITE_DONE && rc!=SQLITE_ROW){
        fprintf(stderr,"FAIL %s: %s\n", what, sqlite3_errmsg(db)); exit(1);
    }
}
static void exec_sql(sqlite3*db, const char*sql){
    char*err=0;
    if(sqlite3_exec(db,sql,0,0,&err)!=SQLITE_OK){
        fprintf(stderr,"FAIL exec [%s]: %s\n", sql, err); exit(1);
    }
}
static unsigned long long rngstate = 88172645463325252ULL;
static void rng_reset(void){ rngstate = 88172645463325252ULL; }
static unsigned long long xrand(void){
    rngstate^=rngstate<<13; rngstate^=rngstate>>7; rngstate^=rngstate<<17; return rngstate;
}
static void open_tuned(sqlite3**db, const char*path, int readonly){
    int flags = readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE);
    if(sqlite3_open_v2(path, db, flags, 0)!=SQLITE_OK){
        fprintf(stderr,"FAIL open: %s\n", sqlite3_errmsg(*db)); exit(1);
    }
    exec_sql(*db, "PRAGMA journal_mode=WAL;");
    exec_sql(*db, "PRAGMA synchronous=NORMAL;");
    exec_sql(*db, "PRAGMA cache_size=-262144;");
    exec_sql(*db, "PRAGMA temp_store=MEMORY;");
    exec_sql(*db, "PRAGMA mmap_size=1073741824;");
}
static void schema(sqlite3*db, int mode){
    if(mode==2){
        exec_sql(db,"CREATE TABLE IF NOT EXISTS objects("
                " id INTEGER PRIMARY KEY, layer INTEGER NOT NULL,"
                " anchor INTEGER NOT NULL, stage INTEGER NOT NULL, geom BLOB NOT NULL);");
        exec_sql(db,"CREATE UNIQUE INDEX IF NOT EXISTS ux_anchor ON objects(layer,anchor);");
    } else {
        exec_sql(db,"CREATE TABLE IF NOT EXISTS objects("
                " id INTEGER PRIMARY KEY, layer INTEGER NOT NULL,"
                " stage INTEGER NOT NULL, geom BLOB NOT NULL);");
    }
    exec_sql(db,"CREATE VIRTUAL TABLE IF NOT EXISTS objects_rtree USING rtree(id,x0,x1,y0,y1);");
    exec_sql(db,"CREATE TABLE IF NOT EXISTS events("
            " seq INTEGER PRIMARY KEY AUTOINCREMENT, kind INTEGER, layer INTEGER,"
            " stage INTEGER, n INTEGER, bx0 INTEGER, bx1 INTEGER, by0 INTEGER, by1 INTEGER);");
}

typedef struct { long long anchor, x, y, w, h; } Rec;
static int cmp_rec(const void*a, const void*b){
    long long d = ((const Rec*)a)->anchor - ((const Rec*)b)->anchor;
    return d<0 ? -1 : (d>0 ? 1 : 0);
}

static int write_bench(const char*path, long long total, int batch, int mode){
    sqlite3*db; open_tuned(&db,path,0); schema(db,mode);
    sqlite3_stmt *ins,*rt,*ev;
    const char* isql = (mode==2)
        ? "INSERT INTO objects(id,layer,anchor,stage,geom) VALUES(?,?,?,?,?)"
        : "INSERT INTO objects(id,layer,stage,geom) VALUES(?,?,?,?)";
    must(sqlite3_prepare_v2(db,isql,-1,&ins,0),db,"prep ins");
    must(sqlite3_prepare_v2(db,"INSERT INTO objects_rtree(id,x0,x1,y0,y1) VALUES(?,?,?,?,?)",-1,&rt,0),db,"prep rt");
    must(sqlite3_prepare_v2(db,"INSERT INTO events(kind,layer,stage,n,bx0,bx1,by0,by1) VALUES(1,?,?,?,?,?,?,?)",-1,&ev,0),db,"prep ev");

    unsigned char*geom=(unsigned char*)malloc(GEOM_BYTES); memset(geom,0xAB,GEOM_BYTES);
    Rec* buf = (Rec*)malloc(sizeof(Rec)*batch);
    long long done=0, collisions=0, seqid=1;
    double t0=now_s(), tmark=t0; long long mark=0;
    int tiles_x=(SLIDE_W+TILE-1)/TILE;
    long long tile=0;

    const char* names[3]={"anchor-pk","anchor-pk-sorted-batch","rowid-pk+unique-anchor"};
    printf("mode=%d (%s) total=%lld batch=%d\n", mode, names[mode], total, batch);
    fflush(stdout);

    while(done<total){
        int n=(int)((total-done)<batch?(total-done):batch);
        /* generate the batch first, so every mode sees the identical anchors */
        for(int i=0;i<n;i++){
            long long tx=(tile%tiles_x)*TILE, ty=(tile/tiles_x)*TILE;
            if(ty>=SLIDE_H) ty=ty%SLIDE_H;
            long long x=tx+(long long)(xrand()%TILE);
            long long y=ty+(long long)(xrand()%TILE);
            if(x>=SLIDE_W)x=SLIDE_W-1;
            if(y>=SLIDE_H)y=SLIDE_H-1;
            buf[i].anchor=((long long)y<<32)|x;
            buf[i].x=x; buf[i].y=y;
            buf[i].w=8+(long long)(xrand()%40);
            buf[i].h=8+(long long)(xrand()%40);
            if((i%64)==63) tile++;
        }
        if(mode==1) qsort(buf,n,sizeof(Rec),cmp_rec);

        exec_sql(db,"BEGIN");
        long long bx0=SLIDE_W,bx1=0,by0=SLIDE_H,by1=0;
        for(int i=0;i<n;i++){
            long long key = (mode==2) ? seqid : buf[i].anchor;
            if(mode==2){
                sqlite3_bind_int64(ins,1,key);      sqlite3_bind_int(ins,2,3);
                sqlite3_bind_int64(ins,3,buf[i].anchor); sqlite3_bind_int(ins,4,1);
                sqlite3_bind_blob (ins,5,geom,GEOM_BYTES,SQLITE_STATIC);
            } else {
                sqlite3_bind_int64(ins,1,key);      sqlite3_bind_int(ins,2,3);
                sqlite3_bind_int (ins,3,1);
                sqlite3_bind_blob(ins,4,geom,GEOM_BYTES,SQLITE_STATIC);
            }
            int rc=sqlite3_step(ins); sqlite3_reset(ins);
            if(rc==SQLITE_CONSTRAINT){ collisions++; continue; }
            if(rc!=SQLITE_DONE) must(rc,db,"step ins");
            if(mode==2) seqid++;

            sqlite3_bind_int64(rt,1,key);
            sqlite3_bind_double(rt,2,(double)buf[i].x);
            sqlite3_bind_double(rt,3,(double)(buf[i].x+buf[i].w));
            sqlite3_bind_double(rt,4,(double)buf[i].y);
            sqlite3_bind_double(rt,5,(double)(buf[i].y+buf[i].h));
            must(sqlite3_step(rt),db,"step rt"); sqlite3_reset(rt);

            if(buf[i].x<bx0)bx0=buf[i].x;
            if(buf[i].x+buf[i].w>bx1)bx1=buf[i].x+buf[i].w;
            if(buf[i].y<by0)by0=buf[i].y;
            if(buf[i].y+buf[i].h>by1)by1=buf[i].y+buf[i].h;
        }
        sqlite3_bind_int(ev,1,3); sqlite3_bind_int(ev,2,1); sqlite3_bind_int(ev,3,n);
        sqlite3_bind_int64(ev,4,bx0); sqlite3_bind_int64(ev,5,bx1);
        sqlite3_bind_int64(ev,6,by0); sqlite3_bind_int64(ev,7,by1);
        must(sqlite3_step(ev),db,"step ev"); sqlite3_reset(ev);
        exec_sql(db,"COMMIT");
        done+=n;
        if(done-mark>=1000000){
            double t=now_s();
            printf("  %6.1fM  %8.0f obj/s (interval)  elapsed %6.1fs\n",
                   done/1e6,(double)(done-mark)/(t-tmark),t-t0);
            fflush(stdout);
            tmark=t; mark=done;
        }
    }
    double t1=now_s();
    printf("INSERT mode=%d %lld attempted, %lld collisions, %.1fs = %.0f obj/s\n",
           mode,done,collisions,t1-t0,(double)done/(t1-t0));
    fflush(stdout);
    sqlite3_finalize(ins); sqlite3_finalize(rt); sqlite3_finalize(ev);
    exec_sql(db,"PRAGMA wal_checkpoint(TRUNCATE);");
    sqlite3_close(db); free(buf); free(geom);
    return 0;
}

static int query_bench(const char*path, int mode){
    sqlite3*db; open_tuned(&db,path,1);
    sqlite3_stmt*q;sqlite3_stmt*qj;
    must(sqlite3_prepare_v2(db,
        "SELECT r.id FROM objects_rtree r WHERE r.x1>=? AND r.x0<=? AND r.y1>=? AND r.y0<=?",-1,&q,0),db,"prep q");
    must(sqlite3_prepare_v2(db,
        "SELECT o.id,o.geom FROM objects_rtree r JOIN objects o ON o.id=r.id "
        "WHERE r.x1>=? AND r.x0<=? AND r.y1>=? AND r.y0<=?",-1,&qj,0),db,"prep qj");
    int sizes[3]; sizes[0]=1024; sizes[1]=4096; sizes[2]=16384;
    for(int s=0;s<3;s++){
        int vp=sizes[s]; int reps=50; long long rows=0; double t0=now_s();
        for(int i=0;i<reps;i++){
            long long x=(long long)(xrand()%(SLIDE_W-vp)), y=(long long)(xrand()%(SLIDE_H-vp));
            sqlite3_bind_double(q,1,(double)x); sqlite3_bind_double(q,2,(double)(x+vp));
            sqlite3_bind_double(q,3,(double)y); sqlite3_bind_double(q,4,(double)(y+vp));
            while(sqlite3_step(q)==SQLITE_ROW) rows++;
            sqlite3_reset(q);
        }
        double t1=now_s();
        printf("  rtree-only  viewport %6dpx  %7.2f ms/query  %9.1f rows/query\n",
               vp,(t1-t0)*1000.0/reps,(double)rows/reps);
        rows=0; t0=now_s();
        for(int i=0;i<reps;i++){
            long long x=(long long)(xrand()%(SLIDE_W-vp)), y=(long long)(xrand()%(SLIDE_H-vp));
            sqlite3_bind_double(qj,1,(double)x); sqlite3_bind_double(qj,2,(double)(x+vp));
            sqlite3_bind_double(qj,3,(double)y); sqlite3_bind_double(qj,4,(double)(y+vp));
            while(sqlite3_step(qj)==SQLITE_ROW) rows++;
            sqlite3_reset(qj);
        }
        t1=now_s();
        printf("  +geom join  viewport %6dpx  %7.2f ms/query  %9.1f rows/query\n",
               vp,(t1-t0)*1000.0/reps,(double)rows/reps);
        fflush(stdout);
    }
    /* get(ObjectId): point lookup by logical identity. Mode 2 pays an index hop. */
    {
        sqlite3_stmt*qp;
        const char* psql = (mode==2)
            ? "SELECT id,geom FROM objects WHERE layer=3 AND anchor=?"
            : "SELECT id,geom FROM objects WHERE id=?";
        must(sqlite3_prepare_v2(db,psql,-1,&qp,0),db,"prep qp");
        int reps=20000; long long hits=0; double t0=now_s();
        for(int i=0;i<reps;i++){
            long long tx=(long long)(xrand()%(SLIDE_W-TILE));
            long long ty=(long long)(xrand()%(SLIDE_H-TILE));
            sqlite3_bind_int64(qp,1,((long long)ty<<32)|tx);
            if(sqlite3_step(qp)==SQLITE_ROW) hits++;
            sqlite3_reset(qp);
        }
        double t1=now_s();
        printf("  get(ObjectId) by anchor  %7.4f ms/lookup  (%lld hits of %d)\n",
               (t1-t0)*1000.0/reps,hits,reps);
        fflush(stdout);
        sqlite3_finalize(qp);
    }
    sqlite3_finalize(q); sqlite3_finalize(qj); sqlite3_close(db);
    return 0;
}

int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"usage: prog <write|query> <db> [total batch mode | mode]\n"); return 2; }
    if(strcmp(argv[1],"write")==0){
        rng_reset();
        return write_bench(argv[2], argc>3?atoll(argv[3]):10000000LL,
                           argc>4?atoi(argv[4]):10000, argc>5?atoi(argv[5]):0);
    }
    if(strcmp(argv[1],"query")==0){
        rng_reset();
        return query_bench(argv[2], argc>3?atoi(argv[3]):0);
    }
    return 2;
}
