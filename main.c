/*
 * ============================================================================
 *  PROJECT LIWA SANDBOX  -  PSP homebrew (PSPSDK / pspdev)
 *  Allegrex @333MHz | 2MB VRAM | 480x272 | 16-bit (5650) double buffered
 *
 *  - Zero malloc/free: every system uses static pools / arenas.
 *  - Fixed-function only. Matrices go through pspgum (VFPU backed).
 *  - Geometry is untextured vertex-colour boxes + a procedural dune mesh, so
 *    the free VRAM (0xCC000 .. 0x200000) stays available for a texture cache.
 *  - HUD text is CPU-drawn into the just-rendered draw buffer using the
 *    8x8 "msx" font that ships inside libpspdebug (no debug-screen conflicts).
 *
 *  World units are metres. +Z is "north" at yaw 0. Right vector = (-cos, 0, sin).
 *  Every gameplay constant is a tunable starting point, not a tested value.
 * ============================================================================
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <psputils.h>
#include <pspiofilemgr.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

PSP_MODULE_INFO("LiwaSandbox", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

extern unsigned char msx[];   /* 8x8 font from libpspdebug */

/* ------------------------------------------------------------------------ */
/*  Hardware / memory constants                                              */
/* ------------------------------------------------------------------------ */
#define BUF_W        512
#define SCR_W        480
#define SCR_H        272
#define FRAME_SIZE   (BUF_W * SCR_H * 2)        /* 0x44000: one 16-bit frame  */
#define DEPTH_OFF    (FRAME_SIZE * 2)           /* depth buffer, 16-bit       */
#define TEX_VRAM_OFF (FRAME_SIZE * 3)           /* 0xCC000: free for textures */
#define VRAM_UNCACHED 0x44000000u

#define PI_F         3.14159265f
#define RGB(r,g,b)   (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define RGB565(r,g,b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

#define SKY_COLOR    RGB(235, 200, 140)

/* Pool sizes (all static) */
#define MAX_VEHICLES  20
#define MAX_NPCS      24
#define MAX_RACE_CKPT 8
#define MAX_CHUNKS    9
#define MAX_PROPS     24
#define MAX_STATIC    16
#define MAX_GALLERY   64

/* World layout */
#define WORLD_HALF    2048.0f
#define CHUNK_SIZE    256.0f
#define CHUNK_COUNT   16

#define CITY_CX  (-1000.0f)
#define CITY_CZ  ( 800.0f)
#define CITY_HX  ( 420.0f)
#define CITY_HZ  ( 420.0f)
#define AIR_CX   ( 1000.0f)
#define AIR_CZ   (-1000.0f)
#define AIR_HX   ( 420.0f)
#define AIR_HZ   ( 140.0f)

#define HEAT_PER_STAR 100.0f
#define MAX_HEAT      599.0f

#define PHOTO_DIR "ms0:/PSP/PHOTO/LIWA"

/* ------------------------------------------------------------------------ */
/*  Enums                                                                    */
/* ------------------------------------------------------------------------ */
typedef enum {
    STATE_MAIN_MENU, STATE_GAMEPLAY, STATE_PHOTO_MODE,
    STATE_INTERIOR, STATE_GALLERY, STATE_CONTROLS_CONFIG
} GameState;

typedef enum { ZONE_DUNES, ZONE_AIRPORT, ZONE_CITY } Zone;

typedef enum {
    VEH_NONE, VEH_LC100, VEH_LC200, VEH_Y60, VEH_Y61, VEH_Y62,
    VEH_PLANE, VEH_DIRT_BIKE, VEH_QUAD,
    VEH_POLICE_SEDAN, VEH_POLICE_SUV, VEH_TACTICAL, VEH_TYPE_COUNT
} VehicleType;

typedef enum { CLS_CAR, CLS_AIR, CLS_BIKE, CLS_QUAD } VehClass;
typedef enum { AI_NONE, AI_CHASE, AI_TACTICAL, AI_ROADBLOCK, AI_ROUTE } VehAI;
typedef enum { STORE_DEALERSHIP, STORE_GUNSHOP, STORE_BANK_VAULT } StoreType;
typedef enum { POI_BANK, POI_POLICE, POI_HOSPITAL, POI_DEALER, POI_GUN, POI_RACE } PoiType;
typedef enum { RESP_NONE, RESP_WASTED, RESP_BUSTED } RespState;
typedef enum { NPC_WANDER, NPC_FLEE } NpcState;

/* ------------------------------------------------------------------------ */
/*  Structures                                                               */
/* ------------------------------------------------------------------------ */
typedef struct { float x, y, z; } Vector3D;

/* Remappable inputs. Defaults follow GTA: Liberty City Stories. */
typedef struct {
    uint32_t accelerate;   /* X : accelerate / sprint           */
    uint32_t brake;        /* O : brake / reverse / fire weapon */
    uint32_t handbrake;    /* [] : handbrake / jump             */
    uint32_t interact;     /* /\ : enter-exit / hijack / shop   */
} ButtonLayout;
typedef ButtonLayout ControlConfig;

/* Static handling table row (per vehicle type) */
typedef struct {
    const char *name;
    VehClass    cls;
    float max_speed;   /* m/s on hard ground                            */
    float accel;       /* m/s^2 (planes: full-throttle thrust)          */
    float weight;      /* kg, used for collision mass ratio             */
    float suspension;  /* stiffness/travel: terrain tracking + landings */
    float traction;    /* 0..1 lateral grip                             */
    float clearance;   /* 0..1: high = handles soft sand well           */
    float turn_rate;   /* rad/s at low speed                            */
    float rollover;    /* lateral-accel threshold before flipping       */
    float stall_speed; /* planes only                                   */
    float hl, hw;      /* half length / half width (planes: wingspan/2) */
    uint32_t color;
    int   price;       /* dealership price, 0 = not for sale            */
} VehSpec;

typedef struct {
    int      type;          /* VehicleType */
    int      active;
    int      ai;            /* VehAI       */
    int      occupied;      /* player inside */
    int      owned;         /* bought or already stolen: no theft heat */
    int      air;           /* airborne (ground vehicles)  */
    int      flipped_now;
    Vector3D pos;           /* pos.y is height of the wheels' ground contact */
    float    yaw, pitch, roll, lean;
    float    vx, vz, vy;    /* world velocity (vy: ballistic / sink)          */
    float    speed;         /* signed forward speed */
    float    spin;          /* collision yaw impulse (pit manoeuvres) */
    float    throttle;      /* planes */
    float    health;
    float    flip_timer;
    int      pinned, route_id, wp;   /* mission vehicle / scripted route */
    float    cruise;
    uint32_t color_override;         /* respray colour, 0 = stock */
} Vehicle;

typedef struct {
    int      active;
    Vector3D pos;
    float    yaw, timer;
    int      health;
    int      state;         /* NpcState */
    uint32_t color;
    int      type;          /* 0 civilian, 1 guard (stationary), 2 mission target */
    int      pinned;        /* mission entity: never auto-despawned */
} NPC;

typedef struct {
    Vector3D pos;
    float    yaw, vy;
    int      health;
    int      on_ground;
    int      veh;           /* -1 when on foot */
    int      weapon;        /* 0 unarmed, 1 pistol, 2 assault rifle, 3 sniper */
    int      owned[4];
    int      ammo[4];
} Player;

typedef struct { float x, z, w, d, h, y; uint32_t color; uint8_t kind, solid; } Prop;

/* Streaming chunk: 3x3 window of these lives in a fixed pool. */
typedef struct {
    int  loaded, cx, cz, zone, nprops;
    Prop props[MAX_PROPS];
} MapChunk;

typedef struct {
    uint8_t wanted_stars;   /* 0..5 */
    float   heat;
    float   unseen, spawn_timer, roadblock_timer, bust;
} WantedSystem;

typedef struct { Vector3D position; int is_triggered; } RaceCheckpoint;
typedef struct { int active, next; float time_left, cooldown; } RaceManager;

typedef struct { PoiType type; const char *name; float x, z, hw, hd, h; uint32_t color; } Poi;
typedef struct { float x, z, hw, hd, h; } Solid;
typedef struct { float steer, accel, brake, pitch, roll, rudder; int handbrake; } VehInput;
typedef struct { uint32_t c; float x, y, z; } CVertex;
typedef struct { char name[32]; int size; } GalleryEntry;

/* ------------------------------------------------------------------------ */
/*  Static tables                                                            */
/* ------------------------------------------------------------------------ */
static const VehSpec VSPEC[VEH_TYPE_COUNT] = {
 /* name, cls, vmax, acc, kg, susp, trac, clr, turn, roll, stall, hl, hw, color, price */
 {"None",               CLS_CAR, 0,0,0,0,0,0,0,0,0,1,1,0,0},
 {"Land Cruiser LC100", CLS_CAR, 40,9.0f,2500,55,0.82f,0.80f,1.50f,44,0,2.4f,1.00f,RGB(235,235,230),0},
 {"Land Cruiser LC200", CLS_CAR, 46,11.0f,2700,60,0.86f,0.85f,1.55f,46,0,2.5f,1.05f,RGB(214,196,160),22000},
 {"Patrol Y60",         CLS_CAR, 36,7.5f,2000,45,0.78f,0.78f,1.45f,40,0,2.3f,0.95f,RGB(150,120,90),0},
 {"Patrol Y61",         CLS_CAR, 42,9.5f,2300,52,0.84f,0.82f,1.50f,42,0,2.4f,1.00f,RGB(225,225,215),12000},
 {"Patrol Y62",         CLS_CAR, 48,12.0f,2650,62,0.88f,0.86f,1.60f,46,0,2.5f,1.05f,RGB(55,60,75),0},
 {"Light Plane",        CLS_AIR, 60,6.0f,1200,30,0.30f,0.90f,0,0,24,4.0f,5.5f,RGB(240,240,245),0},
 {"Dirt Bike",          CLS_BIKE,38,14.0f,120,90,0.90f,0.95f,1.90f,30,0,1.0f,0.30f,RGB(210,60,30),0},
 {"Sport Quad",         CLS_QUAD,32,12.0f,300,85,0.92f,0.92f,1.90f,34,0,1.3f,0.80f,RGB(40,110,190),0},
 {"Police Sedan",       CLS_CAR, 50,12.0f,1800,45,0.88f,0.55f,1.80f,38,0,2.4f,0.95f,RGB(235,235,235),0},
 {"Police SUV",         CLS_CAR, 52,12.0f,2400,55,0.88f,0.82f,1.70f,44,0,2.5f,1.05f,RGB(235,235,240),0},
 {"Tactical Vehicle",   CLS_CAR, 54,13.0f,4000,65,0.92f,0.85f,1.60f,60,0,2.8f,1.15f,RGB(35,40,45),0},
};

/* Points of interest. Building POIs sit on 64 m city cell centres. */
static const Poi POIS[] = {
    {POI_BANK,     "Liwa National Bank",  -928.0f, 800.0f, 22,22,26, RGB(200,170,90)},
    {POI_POLICE,   "Police Department",  -1056.0f, 800.0f, 22,22,22, RGB(60,80,150)},
    {POI_HOSPITAL, "City Hospital",       -800.0f, 672.0f, 22,22,30, RGB(235,235,240)},
    {POI_HOSPITAL, "Oasis Clinic",         300.0f, 400.0f, 18,18,20, RGB(235,235,240)},
    {POI_DEALER,   "Off-Road Dealership", -928.0f, 672.0f, 22,22,14, RGB(200,80,50)},
    {POI_GUN,      "Gun Store",          -1056.0f, 672.0f, 22,22,12, RGB(70,70,70)},
    {POI_RACE,     "Dune Race Start",     -450.0f, 330.0f,  0, 0, 0, RGB(60,220,90)},
};
#define NUM_POIS ((int)(sizeof(POIS) / sizeof(POIS[0])))

typedef struct { float x, z, r; } Pad;
static const Pad PADS[] = { {300.0f, 400.0f, 50.0f}, {-450.0f, 330.0f, 30.0f} };
#define NUM_PADS ((int)(sizeof(PADS) / sizeof(PADS[0])))

static const float RACE_PTS[MAX_RACE_CKPT][2] = {
    {-250, 150}, {100, -50}, {450, -300}, {700, -100},
    {750, 250}, {450, 550}, {50, 700}, {-350, 500}
};

/* ------------------------------------------------------------------------ */
/*  Global state (static arenas)                                             */
/* ------------------------------------------------------------------------ */
static unsigned int __attribute__((aligned(16))) list[262144];   /* 1MB GE list */

static GameState     current_state = STATE_MAIN_MENU;
static Player        player;
static WantedSystem  wanted;
static ButtonLayout  ctl;
static uint32_t      player_money;
static Vehicle       vehicles[MAX_VEHICLES];
static NPC           npcs[MAX_NPCS];
static MapChunk      chunks[MAX_CHUNKS];
static RaceCheckpoint race_cp[MAX_RACE_CKPT];
static RaceManager   race;
static Solid         statics[MAX_STATIC];
static int           n_statics;

static int    running = 1, draw_off = 0, story_mode = 0;
static float  game_time = 0.0f, fire_cd = 0.0f, fire_alert = 0.0f, bank_cooldown = 0.0f;
static int    menu_sel = 0;

static RespState resp_state = RESP_NONE;
static float     resp_timer = 0.0f;
static Vector3D  death_pos;

static Vector3D cam_eye, cam_ctr;
static float    cam_yaw = 0.0f;
/* LCS-style camera state */
static float    look_yaw = 0.0f, look_pitch = 0.0f, l_hold_t = 0.0f, cam_fov = 60.0f, aim_pitch = 0.0f;
static int      cam_mode_foot = 1, cam_mode_veh = 1, fp_mode = 0, lock_idx = -1, lock_active = 0, cam_snap = 0;
static uint32_t g_released = 0;
static const char *WNAME[4] = { "UNARMED", "PISTOL", "ASSAULT RIFLE", "SNIPER RIFLE" };

static Vector3D photo_pos;
static float    photo_yaw = 0.0f, photo_pitch = 0.0f;
static int      capture_req = 0, photo_counter = 1, photo_hide_ui = 0;

static StoreType store_type;
static int   store_poi = 0, store_sel = 0;
static float heist_prog = 0.0f;

static int   ctl_row = 0, ctl_listen = 0;
static GalleryEntry gallery[MAX_GALLERY];
static int   gallery_count = 0, gallery_sel = 0;

static char  status_msg[64];
static float status_timer = 0.0f;

static uint32_t rng_state = 0x1234ABCDu;

/* ------------------------------------------------------------------------ */
/*  Small helpers                                                            */
/* ------------------------------------------------------------------------ */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float wrap_pi(float a) { while (a > PI_F) a -= 2*PI_F; while (a < -PI_F) a += 2*PI_F; return a; }
static uint32_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }
static float rndf(void) { return (float)(rnd() & 0xFFFF) / 65535.0f; }
static uint32_t hash2(int a, int b) {
    uint32_t h = (uint32_t)a * 374761393u + (uint32_t)b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float dist2d(float ax, float az, float bx, float bz) {
    float dx = ax - bx, dz = az - bz; return sqrtf(dx*dx + dz*dz);
}
static float axis(uint8_t v) {                       /* stick -> -1..1, deadzone */
    float d = ((int)v - 128) / 127.0f, a = fabsf(d);
    if (a < 0.18f) return 0.0f;
    a = (a - 0.18f) / 0.82f; if (a > 1.0f) a = 1.0f;
    return d < 0 ? -a : a;
}
static void set_status(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(status_msg, sizeof(status_msg), fmt, ap); va_end(ap);
    status_timer = 3.5f;
}
static uint32_t shade(uint32_t c, float s) {
    uint32_t r = (uint32_t)((c & 0xFF) * s), g = (uint32_t)(((c >> 8) & 0xFF) * s), b = (uint32_t)(((c >> 16) & 0xFF) * s);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* ------------------------------------------------------------------------ */
/*  Terrain: procedural dunes blended into flat city / airport / pads        */
/* ------------------------------------------------------------------------ */
static float smoothstepf(float a, float b, float x) {
    float t = clampf((x - a) / (b - a), 0.0f, 1.0f); return t*t*(3.0f - 2.0f*t);
}
static float rect_flat(float x, float z, float cx, float cz, float hx, float hz, float blend) {
    float dx = fabsf(x - cx) - hx; if (dx < 0) dx = 0;
    float dz = fabsf(z - cz) - hz; if (dz < 0) dz = 0;
    return 1.0f - smoothstepf(0.0f, blend, sqrtf(dx*dx + dz*dz));
}
/* 1 = fully hard/flat ground, 0 = deep sand */
static float flatness(float x, float z) {
    float f = rect_flat(x, z, CITY_CX, CITY_CZ, CITY_HX, CITY_HZ, 160.0f);
    float a = rect_flat(x, z, AIR_CX, AIR_CZ, AIR_HX, AIR_HZ, 120.0f);
    if (a > f) f = a;
    for (int i = 0; i < NUM_PADS; i++) {
        float p = rect_flat(x, z, PADS[i].x, PADS[i].z, PADS[i].r, PADS[i].r, 45.0f);
        if (p > f) f = p;
    }
    return f;
}
static float dune_h(float x, float z) {
    float h = 9.0f * sinf(x*0.011f + 1.7f*sinf(z*0.007f));       /* big swells      */
    h += 6.0f * sinf(z*0.017f + x*0.006f + 1.3f);                 /* cross swells    */
    h += 5.0f * (1.0f - fabsf(sinf(x*0.008f + z*0.013f))) - 2.5f; /* sharp crests    */
    h += 1.2f * sinf(x*0.11f) * sinf(z*0.09f);                    /* ripples         */
    return h;
}
static float terrain_height(float x, float z) { return dune_h(x, z) * (1.0f - flatness(x, z)); }
static int zone_at(float x, float z) {
    if (fabsf(x - AIR_CX) < AIR_HX && fabsf(z - AIR_CZ) < AIR_HZ) return ZONE_AIRPORT;
    if (fabsf(x - CITY_CX) < CITY_HX && fabsf(z - CITY_CZ) < CITY_HZ) return ZONE_CITY;
    return ZONE_DUNES;
}
static int in_city(float x, float z, float margin) {
    return fabsf(x - CITY_CX) < CITY_HX - margin && fabsf(z - CITY_CZ) < CITY_HZ - margin;
}

/* ------------------------------------------------------------------------ */
/*  Static solids + streaming chunks + collision                             */
/* ------------------------------------------------------------------------ */
static void add_static(float x, float z, float hw, float hd, float h) {
    if (n_statics < MAX_STATIC) { statics[n_statics].x = x; statics[n_statics].z = z;
        statics[n_statics].hw = hw; statics[n_statics].hd = hd; statics[n_statics].h = h; n_statics++; }
}
static void build_statics(void) {
    n_statics = 0;
    for (int i = 0; i < NUM_POIS; i++) if (POIS[i].hw > 0) add_static(POIS[i].x, POIS[i].z, POIS[i].hw, POIS[i].hd, POIS[i].h);
    for (int i = 0; i < 3; i++) add_static(850.0f + i*100.0f, -1085.0f, 30, 25, 14);   /* hangars */
    add_static(1150.0f, -1075.0f, 5, 5, 30);                                            /* tower   */
}

static int near_poi_building(float x, float z) {
    for (int i = 0; i < NUM_POIS; i++)
        if (POIS[i].hw > 0 && fabsf(x - POIS[i].x) < 44 && fabsf(z - POIS[i].z) < 44) return 1;
    return 0;
}

static void chunk_load(MapChunk *c, int cx, int cz) {
    c->loaded = 1; c->cx = cx; c->cz = cz; c->nprops = 0;
    float x0 = cx * CHUNK_SIZE - WORLD_HALF, z0 = cz * CHUNK_SIZE - WORLD_HALF;
    c->zone = zone_at(x0 + CHUNK_SIZE*0.5f, z0 + CHUNK_SIZE*0.5f);
    int gx0 = (int)floorf(x0 / 64.0f), gz0 = (int)floorf(z0 / 64.0f);
    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {          /* city blocks */
        int gx = gx0 + i, gz = gz0 + j;
        float bx = gx * 64.0f + 32.0f, bz = gz * 64.0f + 32.0f;
        if (!in_city(bx, bz, 30.0f) || near_poi_building(bx, bz)) continue;
        uint32_t h = hash2(gx, gz);
        if ((h % 100) >= 72 || c->nprops >= MAX_PROPS) continue;
        Prop *p = &c->props[c->nprops++];
        p->x = bx; p->z = bz; p->y = 0; p->kind = 0; p->solid = 1;
        p->w = 28.0f + (float)((h >> 8) % 14); p->d = 28.0f + (float)((h >> 12) % 14);
        p->h = 14.0f + (float)((h >> 16) % 70);
        int t = (int)((h >> 5) % 4);
        p->color = t == 0 ? RGB(170,180,195) : t == 1 ? RGB(205,190,160) : t == 2 ? RGB(150,160,150) : RGB(190,175,150);
    }
    for (int k = 0; k < 6 && c->nprops < MAX_PROPS; k++) {              /* palms / rocks */
        uint32_t h = hash2(cx * 16 + k, cz * 7 + k * 3);
        if ((h % 100) >= 35) continue;
        float px = x0 + (float)((h >> 8) & 255), pz = z0 + (float)((h >> 16) & 255);
        if (flatness(px, pz) > 0.2f) continue;
        Prop *p = &c->props[c->nprops++];
        p->x = px; p->z = pz; p->y = terrain_height(px, pz); p->solid = 1;
        if (((h >> 24) % 4) == 0) { p->kind = 2; p->w = 3.0f; p->d = 3.0f; p->h = 2.5f; p->color = RGB(140,110,80); }
        else                      { p->kind = 1; p->w = 1.2f; p->d = 1.2f; p->h = 7.0f; p->color = RGB(120,85,50); }
    }
}

/* Keep a 3x3 window of chunks resident around (px,pz), reusing pool slots. */
static void update_chunks(float px, float pz) {
    int pcx = (int)floorf((px + WORLD_HALF) / CHUNK_SIZE), pcz = (int)floorf((pz + WORLD_HALF) / CHUNK_SIZE);
    for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) {
        int cx = pcx + dx, cz = pcz + dz;
        if (cx < 0 || cz < 0 || cx >= CHUNK_COUNT || cz >= CHUNK_COUNT) continue;
        int found = 0;
        for (int i = 0; i < MAX_CHUNKS; i++) if (chunks[i].loaded && chunks[i].cx == cx && chunks[i].cz == cz) { found = 1; break; }
        if (found) continue;
        for (int i = 0; i < MAX_CHUNKS; i++) {
            if (!chunks[i].loaded || abs(chunks[i].cx - pcx) > 1 || abs(chunks[i].cz - pcz) > 1) { chunk_load(&chunks[i], cx, cz); break; }
        }
    }
}

static int push_out(Vector3D *p, float r, float cx, float cz, float hw, float hd, float top) {
    if (p->y > top + 2.0f) return 0;
    float qx = clampf(p->x, cx - hw, cx + hw), qz = clampf(p->z, cz - hd, cz + hd);
    float dx = p->x - qx, dz = p->z - qz, d2 = dx*dx + dz*dz;
    if (d2 >= r*r) return 0;
    if (d2 > 1e-6f) { float d = sqrtf(d2), k = (r - d) / d; p->x += dx*k; p->z += dz*k; }
    else {
        float ox = hw + r - fabsf(p->x - cx), oz = hd + r - fabsf(p->z - cz);
        if (ox < oz) p->x += (p->x >= cx ? ox : -ox); else p->z += (p->z >= cz ? oz : -oz);
    }
    return 1;
}
static int collide_world(Vector3D *p, float r) {
    int hit = 0;
    for (int i = 0; i < n_statics; i++) hit |= push_out(p, r, statics[i].x, statics[i].z, statics[i].hw, statics[i].hd, statics[i].h);
    for (int c = 0; c < MAX_CHUNKS; c++) if (chunks[c].loaded)
        for (int i = 0; i < chunks[c].nprops; i++) { const Prop *q = &chunks[c].props[i];
            if (q->solid) hit |= push_out(p, r, q->x, q->z, q->w*0.5f, q->d*0.5f, q->y + q->h); }
    return hit;
}
static int point_blocked(float x, float z) {
    for (int i = 0; i < n_statics; i++) if (fabsf(x - statics[i].x) < statics[i].hw + 3 && fabsf(z - statics[i].z) < statics[i].hd + 3) return 1;
    for (int c = 0; c < MAX_CHUNKS; c++) if (chunks[c].loaded)
        for (int i = 0; i < chunks[c].nprops; i++) { const Prop *q = &chunks[c].props[i];
            if (q->solid && fabsf(x - q->x) < q->w*0.5f + 3 && fabsf(z - q->z) < q->d*0.5f + 3) return 1; }
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  Wanted / crime                                                           */
/* ------------------------------------------------------------------------ */
static void heat_changed(void) {
    wanted.heat = clampf(wanted.heat, 0.0f, MAX_HEAT);
    int s = (int)(wanted.heat / HEAT_PER_STAR); if (s > 5) s = 5;
    wanted.wanted_stars = (uint8_t)s;
}
static void add_heat(float h) { wanted.heat += h; wanted.unseen = 0.0f; heat_changed(); }

/* ------------------------------------------------------------------------ */
/*  Vehicle pool                                                             */
/* ------------------------------------------------------------------------ */
static int alloc_vehicle(void) {
    for (int i = 0; i < MAX_VEHICLES; i++) if (!vehicles[i].active) return i;
    int best = -1; float bd = -1.0f;                      /* reclaim the farthest free one */
    for (int i = 0; i < MAX_VEHICLES; i++) {
        if (i == player.veh || vehicles[i].occupied || vehicles[i].pinned) continue;
        float d = dist2d(vehicles[i].pos.x, vehicles[i].pos.z, player.pos.x, player.pos.z);
        if (d > bd) { bd = d; best = i; }
    }
    return best;
}
static int spawn_vehicle(int type, float x, float z, float yaw, int ai) {
    int i = alloc_vehicle(); if (i < 0) return -1;
    Vehicle *v = &vehicles[i]; memset(v, 0, sizeof(*v));
    v->type = type; v->active = 1; v->ai = ai; v->pos.x = x; v->pos.z = z; v->yaw = yaw;
    v->pos.y = terrain_height(x, z); v->health = 100.0f;
    return i;
}

/* ------------------------------------------------------------------------ */
/*  Vehicle physics                                                          */
/* ------------------------------------------------------------------------ */
static void eject_player(float dmg) {
    if (player.veh < 0) return;
    Vehicle *v = &vehicles[player.veh];
    float rx = -cosf(v->yaw), rz = sinf(v->yaw);
    player.pos.x = v->pos.x + rx * 3.0f; player.pos.z = v->pos.z + rz * 3.0f;
    player.pos.y = terrain_height(player.pos.x, player.pos.z);
    player.vy = 0; player.on_ground = 1; player.yaw = v->yaw;
    player.health -= (int)dmg;
    v->occupied = 0; player.veh = -1;
}

static void update_ground(Vehicle *v, const VehSpec *s, const VehInput *in, float dt) {
    float sand = 1.0f - flatness(v->pos.x, v->pos.z);
    float pen  = sand * (1.0f - s->clearance);          /* low clearance sinks in sand */
    float vmax = s->max_speed * (1.0f - 0.6f * pen);
    float aeff = s->accel * (1.0f - 0.9f * pen);
    float grip = s->traction * (1.0f - 0.7f * pen);
    float fx = sinf(v->yaw), fz = cosf(v->yaw), rx = -fz, rz = fx;
    float vf = v->vx*fx + v->vz*fz, yr = 0.0f;
    int air = v->air;

    if (!air) {                                          /* steering (right = yaw decreases) */
        float sf = clampf(fabsf(vf)/6.0f, 0, 1) / (1.0f + fabsf(vf)/25.0f);
        float dir = vf >= -0.2f ? 1.0f : -1.0f;
        yr = -in->steer * s->turn_rate * sf * dir * (0.6f + 0.4f * grip / s->traction);
        if (in->handbrake) yr *= 1.35f;
    }
    v->yaw += (yr + v->spin) * dt;
    v->spin *= 1.0f / (1.0f + 2.0f*dt);
    fx = sinf(v->yaw); fz = cosf(v->yaw); rx = -fz; rz = fx;
    vf = v->vx*fx + v->vz*fz;
    float vl = v->vx*rx + v->vz*rz;                      /* sideways slip */

    if (!air) {
        if (in->accel > 0.02f) {
            if (vf < -0.5f) vf += 20.0f*dt;
            else vf += aeff * in->accel * (1.0f - clampf(vf/vmax, 0, 1.5f)) * dt;
        }
        if (in->brake > 0.02f) {
            if (vf > 0.5f) vf -= 22.0f*dt;
            else { vf -= aeff*0.6f*dt; if (vf < -vmax*0.35f) vf = -vmax*0.35f; }
        }
        if (in->accel <= 0.02f && in->brake <= 0.02f) vf -= vf * (0.25f + 1.3f*pen) * dt;
        if (in->handbrake) vf -= vf * 1.2f * dt;
        if (vf > vmax) vf -= (vf - vmax) * dt;                       /* leaving hard road into sand */
        vf -= 9.8f * sinf(v->pitch) * 0.7f * dt;                     /* slope gravity */
        float latk = 9.0f * grip * (in->handbrake ? 0.12f : 1.0f);
        vl /= (1.0f + latk*dt);
        v->vx = fx*vf + rx*vl; v->vz = fz*vf + rz*vl;
    }
    v->pos.x += v->vx*dt; v->pos.z += v->vz*dt;
    v->speed = vf;

    /* vertical: follow terrain, leave the ground if it falls away faster than gravity */
    float ground = terrain_height(v->pos.x, v->pos.z);
    if (v->air) {
        v->vy -= 9.8f*dt; v->pos.y += v->vy*dt;
        if (v->pos.y <= ground) {
            float impact = -v->vy;
            if (impact > 12.0f) {
                float dmg = (impact - 12.0f) * 3.0f * (60.0f / s->suspension);
                v->health -= dmg; if (v->occupied) player.health -= (int)(dmg * 0.15f);
            }
            v->air = 0; v->pos.y = ground; v->vy = 0;
        }
    } else {
        float yb = v->pos.y + v->vy*dt - 4.9f*dt*dt;
        if (ground < yb - 0.05f && fabsf(vf) > 5.0f) { v->air = 1; v->vy -= 9.8f*dt; v->pos.y = yb; }
        else {
            float ny = v->pos.y + (ground - v->pos.y) * fminf(1.0f, dt * (8.0f + s->suspension*0.1f));
            v->vy = (ny - v->pos.y) / dt; v->pos.y = ny;
        }
    }

    /* body attitude from terrain normal */
    float pt = 0.0f, rt = 0.0f;
    if (!v->air) {
        float hf = terrain_height(v->pos.x + fx*s->hl, v->pos.z + fz*s->hl);
        float hb = terrain_height(v->pos.x - fx*s->hl, v->pos.z - fz*s->hl);
        float hr = terrain_height(v->pos.x + rx*s->hw, v->pos.z + rz*s->hw);
        float hL = terrain_height(v->pos.x - rx*s->hw, v->pos.z - rz*s->hw);
        pt = atan2f(hf - hb, 2.0f*s->hl); rt = atan2f(hL - hr, 2.0f*s->hw);
    }
    float k = fminf(1.0f, dt * 8.0f);
    v->pitch += (pt - v->pitch) * k; v->roll += (rt - v->roll) * k;
    float lean_t = 0.0f;
    if (s->cls == CLS_BIKE) lean_t = in->steer * 0.45f * clampf(fabsf(vf)/10.0f, 0, 1);
    if (s->cls == CLS_QUAD) lean_t = in->steer * 0.15f * clampf(fabsf(vf)/10.0f, 0, 1);
    v->lean += (lean_t - v->lean) * fminf(1.0f, dt*6.0f);

    /* rollover (bikes/quads flip earliest) */
    if (!v->air && s->rollover > 0 && v->flip_timer <= 0.0f) {
        float alat = fabsf(vf*yr) + fabsf(vl)*2.0f + 15.0f*fabsf(sinf(v->roll));
        if (alat > s->rollover && fabsf(vf) > 8.0f) { v->flip_timer = 3.0f; v->flipped_now = 1; v->health -= 15.0f; }
    }
}

static void update_plane(Vehicle *v, const VehSpec *s, const VehInput *in, float dt) {
    float ground = terrain_height(v->pos.x, v->pos.z);
    int onground = v->pos.y <= ground + 0.4f;
    float speed = v->speed;
    v->throttle = clampf(v->throttle + (in->accel - in->brake) * 0.5f * dt, 0.0f, 1.0f);

    float tr = onground ? 0.0f : in->roll * 0.9f;                /* + = right wing down */
    float tp = in->pitch * 0.6f;                                  /* stick back (down) = nose up */
    if (onground && speed < s->stall_speed * 0.85f) tp = 0.0f;
    v->roll  += (tr - v->roll)  * fminf(1.0f, dt * 2.5f);
    v->pitch += (tp - v->pitch) * fminf(1.0f, dt * 1.8f);
    if (onground && v->pitch < 0.0f) v->pitch = 0.0f;
    float ratio = speed / s->stall_speed;
    if (!onground && ratio < 0.9f) v->pitch -= (0.9f - ratio) * 0.6f * dt;   /* stall: nose drops */
    v->pitch = clampf(v->pitch, -0.9f, 0.9f); v->roll = clampf(v->roll, -1.2f, 1.2f);

    float drag_k = s->accel / (s->max_speed * s->max_speed);
    speed += (s->accel * v->throttle - drag_k * speed * speed) * dt;
    speed -= 9.8f * sinf(v->pitch) * dt;
    if (onground) { speed -= speed * 0.10f * dt; if (in->handbrake) speed -= 12.0f*dt; }
    if (speed < 0.0f) speed = 0.0f;

    float lf = fminf(ratio*ratio, 1.0f) * cosf(v->roll);          /* lift fraction of weight */
    if (lf < 1.0f && !onground) v->vy += (lf - 1.0f) * 9.8f * dt; else v->vy /= (1.0f + 3.0f*dt);
    if (v->vy < -60.0f) v->vy = -60.0f;

    if (!onground) v->yaw += (-sinf(v->roll) * 0.9f * clampf(ratio, 0, 1.2f) - in->rudder * 0.4f) * dt;
    else           v->yaw += (-in->roll * 0.7f * clampf(speed/8.0f, 0, 1) - in->rudder * 0.4f) * dt;

    float cp = cosf(v->pitch), sp = sinf(v->pitch);
    v->pos.x += sinf(v->yaw) * cp * speed * dt;
    v->pos.z += cosf(v->yaw) * cp * speed * dt;
    v->pos.y += (sp * speed + v->vy) * dt;
    if (v->pos.y > 900.0f) v->pos.y = 900.0f;

    ground = terrain_height(v->pos.x, v->pos.z);
    if (v->pos.y <= ground) {
        float descent = -(sp*speed + v->vy);
        if (!onground && (descent > 9.0f || fabsf(v->roll) > 0.5f || v->pitch < -0.3f)) {
            float dmg = 20.0f + fmaxf(0.0f, descent - 9.0f) * 10.0f;
            v->health -= dmg; if (v->occupied) player.health -= (int)(dmg * 0.2f);
        }
        v->pos.y = ground; v->vy = 0.0f;
    }
    v->speed = speed; v->vx = sinf(v->yaw)*cp*speed; v->vz = cosf(v->yaw)*cp*speed;
}

/* Police steering: pursuit + look-ahead obstacle probes. (Open desert, so
 * no nav-mesh: probes push cars around building AABBs.) Tactical units
 * aim for the target's rear quarter so contact spins it (pit manoeuvre). */
static void police_ai(Vehicle *v, VehInput *in) {
    const VehSpec *s = &VSPEC[v->type];
    Vector3D tgt = player.pos; float pspd = 0.0f, pfx = sinf(player.yaw), pfz = cosf(player.yaw);
    if (player.veh >= 0) { Vehicle *pv = &vehicles[player.veh]; tgt = pv->pos; pspd = pv->speed; pfx = sinf(pv->yaw); pfz = cosf(pv->yaw); }
    float dx = tgt.x - v->pos.x, dz = tgt.z - v->pos.z, dist = sqrtf(dx*dx + dz*dz);
    float ax = tgt.x, az = tgt.z;
    if (v->ai == AI_TACTICAL && dist < 70.0f) {
        float rx = -pfz, rz = pfx;
        float side = ((-dx)*rx + (-dz)*rz) >= 0 ? 1.0f : -1.0f;
        ax = tgt.x - pfx*5.0f + rx*side*2.2f; az = tgt.z - pfz*5.0f + rz*side*2.2f;
    }
    float diff = wrap_pi(atan2f(ax - v->pos.x, az - v->pos.z) - v->yaw);
    float ahead = 8.0f + fabsf(v->speed) * 0.6f;
    if (point_blocked(v->pos.x + sinf(v->yaw)*ahead, v->pos.z + cosf(v->yaw)*ahead)) {
        float ly = v->yaw + 0.6f;
        int lb = point_blocked(v->pos.x + sinf(ly)*ahead, v->pos.z + cosf(ly)*ahead);
        diff = lb ? -0.9f : 0.9f;
    }
    in->steer = clampf(-diff * 2.2f, -1.0f, 1.0f);
    float want = s->max_speed;
    if (dist < 45.0f) want = pspd + 4.0f + dist*0.1f;
    if (fabsf(diff) > 0.9f) want *= 0.5f;
    if (v->speed < want) in->accel = 1.0f; else if (v->speed > want + 6.0f) in->brake = 1.0f;
    if (dist < 12.0f && v->ai == AI_CHASE) { in->accel = 0.0f; in->brake = v->speed > pspd + 2.0f ? 1.0f : 0.0f; }
}

static void vehicle_collisions(void) {                       /* circle-vs-circle with mass ratio + pit spin */
    for (int i = 0; i < MAX_VEHICLES; i++) {
        Vehicle *a = &vehicles[i]; if (!a->active || VSPEC[a->type].cls == CLS_AIR) continue;
        for (int j = i + 1; j < MAX_VEHICLES; j++) {
            Vehicle *b = &vehicles[j]; if (!b->active || VSPEC[b->type].cls == CLS_AIR) continue;
            const VehSpec *sa = &VSPEC[a->type], *sb = &VSPEC[b->type];
            float ra = sa->hl*0.8f, rb = sb->hl*0.8f;
            float dx = b->pos.x - a->pos.x, dz = b->pos.z - a->pos.z, d = sqrtf(dx*dx + dz*dz);
            if (d >= ra + rb || d < 0.001f || fabsf(a->pos.y - b->pos.y) > 3.0f) continue;
            float nx = dx/d, nz = dz/d, ma = sa->weight, mb = sb->weight;
            float pen = ra + rb - d;
            a->pos.x -= nx*pen*(mb/(ma+mb)); a->pos.z -= nz*pen*(mb/(ma+mb));
            b->pos.x += nx*pen*(ma/(ma+mb)); b->pos.z += nz*pen*(ma/(ma+mb));
            float rel = (a->vx - b->vx)*nx + (a->vz - b->vz)*nz;
            if (rel <= 0.0f) continue;
            float J = -(1.2f) * (-rel) / (1.0f/ma + 1.0f/mb);
            a->vx -= nx*J/ma; a->vz -= nz*J/ma; b->vx += nx*J/mb; b->vz += nz*J/mb;
            float sxa = nx*(-cosf(a->yaw)) + nz*sinf(a->yaw), sya = nx*sinf(a->yaw) + nz*cosf(a->yaw);
            float sxb = -nx*(-cosf(b->yaw)) - nz*sinf(b->yaw), syb = -nx*sinf(b->yaw) - nz*cosf(b->yaw);
            a->spin = clampf(a->spin + 0.35f*sxa*sya*rel*(mb/(ma+mb)), -3.0f, 3.0f);
            b->spin = clampf(b->spin + 0.35f*sxb*syb*rel*(ma/(ma+mb)), -3.0f, 3.0f);
            a->health -= rel*0.8f*(mb/(ma+mb)); b->health -= rel*0.8f*(ma/(ma+mb));
            if (a->occupied && rel > 8.0f) player.health -= (int)((rel - 8.0f)*0.5f);
            if (b->occupied && rel > 8.0f) player.health -= (int)((rel - 8.0f)*0.5f);
        }
    }
}

/* ------------------------------------------------------------------------ */
/*  Player / weapons / interactions                                          */
/* ------------------------------------------------------------------------ */
static int nearest_poi_of(PoiType t, float x, float z) {
    int best = -1; float bd = 1e30f;
    for (int i = 0; i < NUM_POIS; i++) if (POIS[i].type == t) {
        float d = dist2d(x, z, POIS[i].x, POIS[i].z); if (d < bd) { bd = d; best = i; } }
    return best;
}
/* Closest enterable shop whose footprint edge is within 8 m of the player. */
static int shop_in_reach(void) {
    for (int i = 0; i < NUM_POIS; i++) {
        PoiType t = POIS[i].type; if (t != POI_BANK && t != POI_DEALER && t != POI_GUN) continue;
        float dx = fabsf(player.pos.x - POIS[i].x) - POIS[i].hw; if (dx < 0) dx = 0;
        float dz = fabsf(player.pos.z - POIS[i].z) - POIS[i].hd; if (dz < 0) dz = 0;
        if (sqrtf(dx*dx + dz*dz) < 8.0f) return i;
    }
    return -1;
}
static void enter_vehicle(int i) {
    Vehicle *v = &vehicles[i];
    if (v->ai == AI_ROUTE) v->ai = AI_NONE;                          /* mission vehicle: no heat */
    else if (v->ai != AI_NONE) { add_heat(60.0f); v->ai = AI_NONE; } /* jacked a cop car */
    else if (!v->owned) { add_heat(12.0f); v->owned = 1; }           /* grand theft */
    if (v->flip_timer > 0) return;
    v->occupied = 1; player.veh = i;
}
static void fire_weapon(float yaw) {
    static const float DMG[4] = {0, 25, 12, 100}, RANGE[4] = {0, 70, 100, 300}, CD[4] = {0, 0.35f, 0.10f, 1.2f}, HEAT[4] = {0, 3, 3, 10};
    int w = player.weapon;
    float dmg = DMG[w], range = RANGE[w];
    player.ammo[w]--; fire_cd = CD[w]; fire_alert = 3.0f; add_heat(HEAT[w]);
    float fx = sinf(yaw), fz = cosf(yaw), bestt = range; int kind = 0, idx = -1;
    for (int i = 0; i < MAX_NPCS; i++) if (npcs[i].active) {
        float dx = npcs[i].pos.x - player.pos.x, dz = npcs[i].pos.z - player.pos.z;
        float t = dx*fx + dz*fz, perp = fabsf(dx*fz - dz*fx);
        if (t > 0 && t < bestt && perp < 1.2f) { bestt = t; kind = 1; idx = i; }
    }
    for (int i = 0; i < MAX_VEHICLES; i++) if (vehicles[i].active && i != player.veh) {
        float dx = vehicles[i].pos.x - player.pos.x, dz = vehicles[i].pos.z - player.pos.z;
        float t = dx*fx + dz*fz, perp = fabsf(dx*fz - dz*fx);
        if (t > 0 && t < bestt && perp < 2.2f) { bestt = t; kind = 2; idx = i; }
    }
    if (kind == 1) { npcs[idx].health -= (int)dmg; npcs[idx].state = NPC_FLEE;
        if (npcs[idx].health <= 0) { npcs[idx].active = 0; add_heat(60.0f); } }
    else if (kind == 2) vehicles[idx].health -= dmg * 0.5f;
}

/* Target lock-on (hold R on foot). Index space: NPCs first, then police vehicles. */
#define LOCK_N     (MAX_NPCS + MAX_VEHICLES)
#define LOCK_RANGE 60.0f
static int lock_pos(int idx, float *x, float *z) {
    if (idx < 0 || idx >= LOCK_N) return 0;
    if (idx < MAX_NPCS) { if (!npcs[idx].active) return 0; *x = npcs[idx].pos.x; *z = npcs[idx].pos.z; }
    else {
        int k = idx - MAX_NPCS; const Vehicle *v = &vehicles[k];
        if (!v->active || k == player.veh || v->type < VEH_POLICE_SEDAN) return 0;
        *x = v->pos.x; *z = v->pos.z;
    }
    return dist2d(*x, *z, player.pos.x, player.pos.z) < LOCK_RANGE;
}
static int lock_nearest(void) {
    int best = -1; float bd = 1e30f, x, z;
    for (int i = 0; i < LOCK_N; i++) if (lock_pos(i, &x, &z)) {
        float d = dist2d(x, z, player.pos.x, player.pos.z); if (d < bd) { bd = d; best = i; } }
    return best;
}
static void lock_cycle(int dir) {
    float x, z;
    for (int k = 1; k <= LOCK_N; k++) {
        int i = ((lock_idx + dir*k) % LOCK_N + LOCK_N) % LOCK_N;
        if (lock_pos(i, &x, &z)) { lock_idx = i; return; }
    }
}

static void player_on_foot(float lx, float ly, uint32_t b, uint32_t pressed, float dt, int rheld) {
    float spd = (b & ctl.accelerate) ? 7.0f : 3.5f, tx, tz;
    if (lock_active && lock_pos(lock_idx, &tx, &tz)) {          /* lock-on: face target, strafe */
        player.yaw = atan2f(tx - player.pos.x, tz - player.pos.z);
        float fwd = -ly * spd, str = lx * spd * 0.8f;
        player.pos.x += (sinf(player.yaw) * fwd - cosf(player.yaw) * str) * dt;
        player.pos.z += (cosf(player.yaw) * fwd + sinf(player.yaw) * str) * dt;
    } else {
        player.yaw -= lx * 2.4f * dt;
        float mv = -ly * spd;
        player.pos.x += sinf(player.yaw) * mv * dt; player.pos.z += cosf(player.yaw) * mv * dt;
    }
    if ((pressed & ctl.handbrake) && player.on_ground) { player.vy = 5.5f; player.on_ground = 0; }
    player.vy -= 15.0f * dt; player.pos.y += player.vy * dt;
    float g = terrain_height(player.pos.x, player.pos.z);
    if (player.pos.y <= g) { player.pos.y = g; player.vy = 0; player.on_ground = 1; }
    collide_world(&player.pos, 0.5f);
    player.pos.x = clampf(player.pos.x, -2000, 2000); player.pos.z = clampf(player.pos.z, -2000, 2000);

    if (!fp_mode) {
        if (rheld) {                                              /* D-pad cycles lock-on targets */
            if (pressed & PSP_CTRL_RIGHT) lock_cycle(1);
            if (pressed & PSP_CTRL_LEFT)  lock_cycle(-1);
        } else {                                                  /* otherwise cycles weapons */
            if (pressed & PSP_CTRL_RIGHT) { do player.weapon = (player.weapon + 1) % 4; while (!player.owned[player.weapon]); }
            if (pressed & PSP_CTRL_LEFT)  { do player.weapon = (player.weapon + 3) % 4; while (!player.owned[player.weapon]); }
        }
    }
    if ((b & ctl.brake) && player.weapon > 0 && fire_cd <= 0.0f && player.ammo[player.weapon] > 0) fire_weapon(player.yaw);

    if (pressed & ctl.interact) {
        int shop = shop_in_reach();
        if (shop >= 0) {
            store_poi = shop; store_sel = 0; heist_prog = 0.0f;
            store_type = POIS[shop].type == POI_BANK ? STORE_BANK_VAULT : POIS[shop].type == POI_DEALER ? STORE_DEALERSHIP : STORE_GUNSHOP;
            current_state = STATE_INTERIOR;
        } else {
            int best = -1; float bd = 6.0f;
            for (int i = 0; i < MAX_VEHICLES; i++) if (vehicles[i].active && !vehicles[i].occupied) {
                float d = dist2d(vehicles[i].pos.x, vehicles[i].pos.z, player.pos.x, player.pos.z);
                if (d < bd) { bd = d; best = i; } }
            if (best >= 0) enter_vehicle(best);
        }
    }
}

/* Scripted routes for mission vehicles (static pool, no malloc). */
#define MAX_ROUTES 4
#define ROUTE_PTS  8
typedef struct { Vector3D p[ROUTE_PTS]; int n; } Route;
static Route routes[MAX_ROUTES];
static int   routes_used = 0;

static void route_ai(Vehicle *v, VehInput *in) {
    if (v->route_id <= 0) return;
    const Route *r = &routes[v->route_id - 1];
    if (v->wp >= r->n) { in->brake = v->speed > 0.5f ? 1.0f : 0.0f; return; }      /* route finished: stop */
    float dx = r->p[v->wp].x - v->pos.x, dz = r->p[v->wp].z - v->pos.z;
    if (sqrtf(dx*dx + dz*dz) < 14.0f) { v->wp++; return; }
    float diff = wrap_pi(atan2f(dx, dz) - v->yaw), ahead = 8.0f + fabsf(v->speed) * 0.6f;
    if (point_blocked(v->pos.x + sinf(v->yaw)*ahead, v->pos.z + cosf(v->yaw)*ahead)) {
        float ly = v->yaw + 0.6f;
        diff = point_blocked(v->pos.x + sinf(ly)*ahead, v->pos.z + cosf(ly)*ahead) ? -0.9f : 0.9f;
    }
    in->steer = clampf(-diff * 2.2f, -1.0f, 1.0f);
    float want = v->cruise; if (fabsf(diff) > 0.7f) want *= 0.5f;
    if (v->speed < want) in->accel = 0.8f; else if (v->speed > want + 3.0f) in->brake = 1.0f;
}

static void update_vehicles(float dt, float lx, float ly, uint32_t b, uint32_t pressed, float ext_l, float ext_r) {
    for (int i = 0; i < MAX_VEHICLES; i++) {
        Vehicle *v = &vehicles[i]; if (!v->active) continue;
        const VehSpec *s = &VSPEC[v->type];
        VehInput in; memset(&in, 0, sizeof(in));
        if (i == player.veh) {
            in.steer = lx; in.roll = lx; in.pitch = ly;
            in.accel = (b & ctl.accelerate) ? 1.0f : 0.0f; in.brake = (b & ctl.brake) ? 1.0f : 0.0f;
            in.handbrake = (b & ctl.handbrake) ? 1 : 0; in.rudder = ext_r - ext_l;
        } else if (v->ai == AI_CHASE || v->ai == AI_TACTICAL) police_ai(v, &in);
        else if (v->ai == AI_ROUTE) route_ai(v, &in);

        if (v->flip_timer > 0.0f) {
            v->flip_timer -= dt; v->vx *= 0.9f; v->vz *= 0.9f; v->speed = 0; memset(&in, 0, sizeof(in));
            if (v->flip_timer <= 0.0f) { v->roll = 0; v->pitch = 0; }
        }
        if (s->cls == CLS_AIR) update_plane(v, s, &in, dt);
        else {
            if (v->flip_timer > 0.0f) { v->pos.x += v->vx*dt; v->pos.z += v->vz*dt; }
            else update_ground(v, s, &in, dt);
        }
        v->pos.x = clampf(v->pos.x, -2000, 2000); v->pos.z = clampf(v->pos.z, -2000, 2000);

        float hitspeed = fabsf(v->speed);
        if (collide_world(&v->pos, s->hl * 0.7f)) {
            if (s->cls == CLS_AIR) v->health -= 100.0f;
            else { if (hitspeed > 8.0f) { v->health -= hitspeed * 1.2f; if (v->occupied) player.health -= (int)(hitspeed * 0.25f); }
                   v->vx *= 0.25f; v->vz *= 0.25f; }
        }
        if (v->flipped_now) { v->flipped_now = 0; if (i == player.veh) eject_player(25.0f); }
        if (v->health <= 0.0f) {                                            /* wrecked */
            if (i == player.veh) eject_player(50.0f);
            if (v->ai != AI_NONE) add_heat(80.0f);
            v->active = 0; v->occupied = 0;
        }
        if (i == player.veh && (pressed & ctl.interact)) {                  /* exit */
            int ok = (s->cls == CLS_AIR) ? (v->pos.y <= terrain_height(v->pos.x, v->pos.z) + 0.6f && v->speed < 6.0f)
                                         : fabsf(v->speed) < 12.0f;
            if (ok) eject_player(0.0f);
        }
    }
    if (player.veh >= 0) { Vehicle *pv = &vehicles[player.veh]; player.pos = pv->pos; player.yaw = pv->yaw; }
    vehicle_collisions();
}

/* ------------------------------------------------------------------------ */
/*  Pedestrians                                                              */
/* ------------------------------------------------------------------------ */
static void update_npcs(float dt) {
    int spawned = 0;
    for (int i = 0; i < MAX_NPCS; i++) {
        NPC *n = &npcs[i];
        if (!n->active) {
            if (spawned || rndf() > 0.03f) continue;
            float a = rndf()*2*PI_F, r = 80.0f + rndf()*120.0f;
            float x = player.pos.x + sinf(a)*r, z = player.pos.z + cosf(a)*r;
            if (!in_city(x, z, 20.0f) || point_blocked(x, z)) continue;
            memset(n, 0, sizeof(*n)); n->active = 1; n->pos.x = x; n->pos.z = z; n->health = 50; n->yaw = rndf()*6.28f;
            n->color = RGB(80 + (rnd()%150), 80 + (rnd()%150), 80 + (rnd()%150)); spawned = 1; continue;
        }
        float dx = player.pos.x - n->pos.x, dz = player.pos.z - n->pos.z, d = sqrtf(dx*dx + dz*dz);
        if (d > 260.0f && !n->pinned) { n->active = 0; continue; }
        float pspd = player.veh >= 0 ? fabsf(vehicles[player.veh].speed) : 0.0f;
        if (n->type == 0 && ((fire_alert > 0.0f && d < 60.0f) || (pspd > 15.0f && d < 25.0f))) n->state = NPC_FLEE;
        float sp = (n->type == 1) ? 0.0f : (n->type == 2) ? 0.9f : 1.4f;
        if (n->state == NPC_FLEE) { n->yaw = atan2f(-dx, -dz); sp = 5.0f; }
        else { n->timer -= dt; if (n->timer <= 0.0f) { n->yaw += (rndf() - 0.5f) * 2.5f; n->timer = 1.0f + rndf()*3.0f; } }
        n->pos.x += sinf(n->yaw)*sp*dt; n->pos.z += cosf(n->yaw)*sp*dt;
        if (collide_world(&n->pos, 0.4f)) n->yaw += PI_F * 0.6f;
        n->pos.y = terrain_height(n->pos.x, n->pos.z);
        if (player.veh >= 0 && d < 3.0f && pspd > 7.0f) { n->active = 0; add_heat(40.0f); }   /* hit and run */
    }
}

/* ------------------------------------------------------------------------ */
/*  Wanted system: police spawning, roadblocks, decay, busted                */
/* ------------------------------------------------------------------------ */
static void update_wanted(float dt) {
    int stars = wanted.wanted_stars, chasers = 0, tact = 0, blocks = 0;
    float nearest = 1e30f;
    for (int i = 0; i < MAX_VEHICLES; i++) {
        Vehicle *v = &vehicles[i]; if (!v->active) continue;
        float d = dist2d(v->pos.x, v->pos.z, player.pos.x, player.pos.z);
        if (v->ai == AI_CHASE || v->ai == AI_TACTICAL) {
            if (stars == 0) { v->ai = AI_NONE; continue; }
            chasers++; if (v->ai == AI_TACTICAL) tact++;
            if (d < nearest) nearest = d;
            if (d > 700.0f) v->active = 0;
        } else if (v->ai == AI_ROADBLOCK) {
            blocks++; if (stars < 3 || d > 450.0f) { v->active = 0; blocks--; }
        } else if (v->type >= VEH_POLICE_SEDAN && !v->occupied && d > 250.0f && stars == 0) v->active = 0;
    }
    if (stars == 0) { wanted.bust = 0; return; }

    wanted.spawn_timer -= dt; wanted.roadblock_timer -= dt;
    if (chasers < stars && wanted.spawn_timer <= 0.0f) {                       /* ★1-2: cars, ★4-5: tactical */
        float a = rndf()*2*PI_F, r = 170.0f + rndf()*90.0f;
        float x = clampf(player.pos.x + sinf(a)*r, -1990, 1990), z = clampf(player.pos.z + cosf(a)*r, -1990, 1990);
        if (!point_blocked(x, z)) {
            int type = VEH_POLICE_SEDAN, ai = AI_CHASE;
            if (stars >= 4 && tact < stars - 3) { type = VEH_TACTICAL; ai = AI_TACTICAL; }
            else if (stars >= 2 && (rnd() & 1)) type = VEH_POLICE_SUV;
            int k = spawn_vehicle(type, x, z, atan2f(player.pos.x - x, player.pos.z - z), ai);
            if (k >= 0) vehicles[k].owned = 1;
            wanted.spawn_timer = 2.5f;
        }
    }
    if (stars >= 3 && blocks < 2 && wanted.roadblock_timer <= 0.0f) {          /* ★3+: roadblock ahead */
        float h = player.veh >= 0 ? vehicles[player.veh].yaw : player.yaw;
        float x = player.pos.x + sinf(h)*220.0f, z = player.pos.z + cosf(h)*220.0f;
        if (fabsf(x) < 1900 && fabsf(z) < 1900 && !point_blocked(x, z)) {
            for (int k = -1; k <= 1; k += 2) {
                int idx = spawn_vehicle(VEH_POLICE_SEDAN, x + cosf(h)*4.5f*k, z - sinf(h)*4.5f*k, h + PI_F*0.5f, AI_ROADBLOCK);
                if (idx >= 0) vehicles[idx].owned = 1;
            }
            wanted.roadblock_timer = 25.0f;
        }
    }
    if (nearest < 110.0f) wanted.unseen = 0.0f; else wanted.unseen += dt;      /* evade */
    if (wanted.unseen > 8.0f) { wanted.heat -= 6.0f*dt; heat_changed(); }

    if (stars >= 2 && nearest < 40.0f) {                                       /* police gunfire abstraction */
        float dmg = 2.5f * stars * dt;
        if (player.veh >= 0) vehicles[player.veh].health -= dmg * 2.0f; else player.health -= (int)ceilf(dmg);
    }
    int stopped = player.veh < 0 || fabsf(vehicles[player.veh].speed) < 4.0f;
    if (nearest < 6.0f && stopped) wanted.bust += dt; else wanted.bust = fmaxf(0.0f, wanted.bust - dt);
}

static void begin_resp(RespState st) {
    if (resp_state != RESP_NONE) return;
    if (player.veh >= 0) eject_player(0.0f);
    resp_state = st; resp_timer = 2.8f; death_pos = player.pos;
}
static void do_respawn(void) {
    uint32_t pen = (resp_state == RESP_WASTED) ? 500u : 1000u;
    player_money = player_money > pen ? player_money - pen : 0;
    int poi = nearest_poi_of(resp_state == RESP_WASTED ? POI_HOSPITAL : POI_POLICE, death_pos.x, death_pos.z);
    if (poi >= 0) { player.pos.x = POIS[poi].x; player.pos.z = POIS[poi].z - 32.0f; }
    player.pos.y = terrain_height(player.pos.x, player.pos.z); player.yaw = 0.0f; player.vy = 0;
    player.health = (resp_state == RESP_WASTED) ? 100 : 60;
    if (resp_state == RESP_BUSTED) { player.weapon = 0; for (int w = 1; w < 4; w++) { player.owned[w] = 0; player.ammo[w] = 0; } }
    memset(&wanted, 0, sizeof(wanted)); race.active = 0;
    for (int i = 0; i < MAX_VEHICLES; i++) if (vehicles[i].active && vehicles[i].ai != AI_NONE) vehicles[i].active = 0;
    cam_yaw = player.yaw;
    set_status(resp_state == RESP_WASTED ? "Treated at %s. Fee $%u" : "Released from %s. Fine $%u",
               poi >= 0 ? POIS[poi].name : "?", (unsigned)pen);
    resp_state = RESP_NONE;
}

/* ------------------------------------------------------------------------ */
/*  Race manager                                                             */
/* ------------------------------------------------------------------------ */
static void update_race(float dt) {
    if (race.cooldown > 0.0f) race.cooldown -= dt;
    int sp = nearest_poi_of(POI_RACE, 0, 0);
    if (!race.active) {
        if (sp >= 0 && player.veh >= 0 && race.cooldown <= 0.0f && wanted.wanted_stars == 0 &&
            dist2d(player.pos.x, player.pos.z, POIS[sp].x, POIS[sp].z) < 20.0f) {
            race.active = 1; race.next = 0; race.time_left = 50.0f;
            for (int i = 0; i < MAX_RACE_CKPT; i++) race_cp[i].is_triggered = 0;
            set_status("Dune race started! Hit all %d checkpoints", MAX_RACE_CKPT);
        }
        return;
    }
    race.time_left -= dt;
    if (race.time_left <= 0.0f) { race.active = 0; race.cooldown = 20.0f; set_status("Race failed - out of time"); return; }
    if (player.veh >= 0 && dist2d(player.pos.x, player.pos.z, race_cp[race.next].position.x, race_cp[race.next].position.z) < 28.0f) {
        race_cp[race.next].is_triggered = 1; race.next++; race.time_left += 12.0f;
        if (race.next >= MAX_RACE_CKPT) {
            uint32_t pay = 6000u + (uint32_t)(race.time_left * 80.0f);
            player_money += pay; race.active = 0; race.cooldown = 30.0f; set_status("Race won! +$%u", (unsigned)pay);
        }
    }
}

/* ------------------------------------------------------------------------ */
/*  Camera                                                                   */
/* ------------------------------------------------------------------------ */
/* LCS-style camera. Auto-follow by default; L = free look (tap = re-centre);
 * R = lock-on (foot) / aim (sniper first-person); SELECT cycles distance. */
static void camera_follow(float dt, float lx, float ly, int lheld) {
    static const float FD[3] = {4.5f, 7.0f, 11.0f}, FH[3] = {2.0f, 3.0f, 5.0f};
    static const float VD[3] = {9.0f, 14.0f, 22.0f}, VH[3] = {3.5f, 5.0f, 8.0f};
    float tyaw = player.yaw, dist, h, tx, tz; Vector3D tp = player.pos;
    const VehSpec *s = NULL; const Vehicle *v = NULL; int bumper = 0;

    if (fp_mode) {                                               /* first-person precision aim */
        cam_yaw = player.yaw; look_yaw = look_pitch = 0.0f;
        float cp = cosf(aim_pitch);
        cam_eye.x = player.pos.x; cam_eye.y = player.pos.y + 1.65f; cam_eye.z = player.pos.z;
        cam_ctr.x = cam_eye.x + sinf(player.yaw)*cp*10.0f; cam_ctr.y = cam_eye.y + sinf(aim_pitch)*10.0f; cam_ctr.z = cam_eye.z + cosf(player.yaw)*cp*10.0f;
        return;
    }
    if (player.veh >= 0) {
        v = &vehicles[player.veh]; s = &VSPEC[v->type]; tyaw = v->yaw; tp = v->pos;
        bumper = (cam_mode_veh == 3);
        int m = bumper ? 1 : cam_mode_veh; float k = (s->cls == CLS_AIR) ? 1.8f : 1.0f;
        dist = VD[m] * k; h = VH[m] * k;
    } else { dist = FD[cam_mode_foot]; h = FH[cam_mode_foot]; }

    if (lock_active && lock_pos(lock_idx, &tx, &tz)) {            /* R lock-on camera */
        cam_yaw += wrap_pi(atan2f(tx - player.pos.x, tz - player.pos.z) - cam_yaw) * fminf(1.0f, dt * 8.0f);
        look_yaw = look_pitch = 0.0f;
    } else {
        if (cam_snap) { cam_yaw = tyaw; look_yaw = look_pitch = 0.0f; cam_snap = 0; }       /* L tap */
        else cam_yaw += wrap_pi(tyaw - cam_yaw) * fminf(1.0f, dt * (bumper ? 12.0f : (s && s->cls == CLS_AIR) ? 2.0f : 4.0f));
        if (lheld) {
            if (v) {                                              /* L + stick: window / behind look */
                float t = lx < -0.5f ? PI_F*0.5f : lx > 0.5f ? -PI_F*0.5f : (ly > 0.5f ? PI_F : 0.0f);
                look_yaw += wrap_pi(t - look_yaw) * fminf(1.0f, dt * 8.0f);
                look_pitch /= (1.0f + 3.0f*dt);
            } else {                                              /* on foot: pan / tilt / pull down = behind */
                look_yaw = wrap_pi(look_yaw - lx * 2.6f * dt);
                look_pitch = clampf(look_pitch + ly * 1.2f * dt, -0.6f, 0.9f);
                if (ly > 0.8f && fabsf(lx) < 0.5f) look_yaw += wrap_pi(PI_F - look_yaw) * fminf(1.0f, dt * 10.0f);
            }
        } else {                                                  /* release: drift back behind the player */
            look_yaw += wrap_pi(-look_yaw) * fminf(1.0f, dt * 2.5f);
            look_pitch /= (1.0f + 2.5f*dt);
        }
    }

    float vyaw = cam_yaw + look_yaw, dx = sinf(vyaw), dz = cosf(vyaw);
    float inside = v ? clampf(fabsf(look_yaw) / (PI_F*0.5f), 0.0f, 1.0f) : 0.0f;
    if (bumper || inside > 0.3f) {                                /* driver / bumper viewpoint */
        float fwd = bumper ? s->hl + 0.3f : 0.0f;
        cam_eye.x = tp.x + sinf(tyaw)*fwd; cam_eye.y = tp.y + (bumper ? 1.0f : 1.6f); cam_eye.z = tp.z + cosf(tyaw)*fwd;
        cam_ctr.x = cam_eye.x + dx*10.0f; cam_ctr.z = cam_eye.z + dz*10.0f;
        cam_ctr.y = cam_eye.y + (bumper ? sinf(v->pitch)*10.0f : 0.0f);
    } else {
        float r = sqrtf(dist*dist + h*h), el = clampf(atan2f(h, dist) + look_pitch, -0.3f, 1.4f);
        cam_eye.x = tp.x - dx*r*cosf(el); cam_eye.z = tp.z - dz*r*cosf(el); cam_eye.y = tp.y + r*sinf(el);
        cam_ctr.x = tp.x; cam_ctr.y = tp.y + 1.5f; cam_ctr.z = tp.z;
    }
    float g = terrain_height(cam_eye.x, cam_eye.z) + 1.0f; if (cam_eye.y < g) cam_eye.y = g;
}

/* ------------------------------------------------------------------------ */
/*  Mission system: deterministic FSM + mission event API                    */
/* ------------------------------------------------------------------------ */
typedef Vector3D Vector3;
typedef int EntityID;      /* NPC: 0..MAX_NPCS-1, vehicle: ENT_VEH_BASE + pool index, -1 = invalid */
typedef int VehicleID;
typedef int BlipID;
#define ENT_VEH_BASE   100
#define MAX_BLIPS      8
#define MAX_MISSIONS   3
#define NPC_T_CIVILIAN 0
#define NPC_T_GUARD    1
#define NPC_T_TARGET   2
enum { BLIP_YELLOW, BLIP_RED, BLIP_GREEN, BLIP_BLUE };
enum { BLIPT_OBJECTIVE, BLIPT_TARGET, BLIPT_DESTINATION };

typedef enum { MS_UNTRIGGERED, MS_INIT, MS_STEP_IN_PROGRESS, MS_STEP_COMPLETE, MS_SUCCESS, MS_FAILED } MissionState;
typedef struct {
    const char *name; Vector3 trigger; uint32_t reward;
    void (*start)(void); void (*update)(float dt); void (*cleanup)(void);   /* Mission_Start / _Update / _CleanUp */
} MissionDef;
typedef struct { int active; Vector3 pos; int type, color; } Blip;

static Blip         blips[MAX_BLIPS];
static MissionState mission_state = MS_UNTRIGGERED;
static int  mission_active = -1, mission_step = 0, mission_step_entered = 0, mission_done[MAX_MISSIONS], player_outfit = 0;
static char objective_text[64], banner_text[64], fail_reason[48];
static float banner_timer = 0.0f;
static struct { int active, countup; float t; } mtimer;
static struct { int active; EntityID target; float min_d, max_d, value; } tail;
static struct { int armed; EntityID subject; int taken; } mphoto;
static const uint32_t BLIP_RGB[4]    = { RGB(255,210,40), RGB(255,50,50), RGB(60,230,90), RGB(70,140,255) };
static const uint32_t OUTFIT_RGB[4]  = { RGB(230,200,40), RGB(245,245,245), RGB(25,25,30), RGB(160,140,100) };
static const uint32_t RESPRAY_RGB[6] = { RGB(200,30,30), RGB(30,60,170), RGB(20,20,20), RGB(240,240,240), RGB(40,140,60), RGB(230,150,20) };

static void banner(const char *t, float secs) { snprintf(banner_text, sizeof(banner_text), "%s", t); banner_timer = secs; }
static int ent_pos(EntityID e, Vector3 *out) {
    if (e >= ENT_VEH_BASE && e < ENT_VEH_BASE + MAX_VEHICLES) { const Vehicle *v = &vehicles[e - ENT_VEH_BASE]; if (!v->active) return 0; *out = v->pos; return 1; }
    if (e >= 0 && e < MAX_NPCS && npcs[e].active) { *out = npcs[e].pos; return 1; }
    return 0;
}

/* ---- Event API (as specified) ---- */
EntityID SpawnNPC(int npcTypeID, Vector3 position, float rotation) {
    for (int i = 0; i < MAX_NPCS; i++) if (!npcs[i].active) {
        NPC *n = &npcs[i]; memset(n, 0, sizeof(*n));
        n->active = 1; n->pinned = 1; n->type = npcTypeID; n->health = 100; n->yaw = rotation;
        n->pos.x = position.x; n->pos.z = position.z; n->pos.y = terrain_height(position.x, position.z);
        n->color = npcTypeID == NPC_T_TARGET ? RGB(240,210,90) : npcTypeID == NPC_T_GUARD ? RGB(40,40,50) : RGB(150,150,150);
        return i;
    }
    return -1;                                   /* pool full */
}
VehicleID SpawnVehicle(int vehicleTypeID, Vector3 position, float rotation) {
    int i = spawn_vehicle(vehicleTypeID, position.x, position.z, rotation, AI_NONE);
    if (i < 0) return -1;
    vehicles[i].pinned = 1; vehicles[i].owned = 1;
    return ENT_VEH_BASE + i;
}
void RemoveEntity(EntityID e) {
    if (e >= ENT_VEH_BASE && e < ENT_VEH_BASE + MAX_VEHICLES) {
        int i = e - ENT_VEH_BASE; if (player.veh == i) eject_player(0.0f);
        vehicles[i].active = 0; vehicles[i].pinned = 0; vehicles[i].occupied = 0;
    } else if (e >= 0 && e < MAX_NPCS) { npcs[e].active = 0; npcs[e].pinned = 0; }
}
BlipID AddRadarBlip(Vector3 position, int blipType, int color) {
    for (int i = 0; i < MAX_BLIPS; i++) if (!blips[i].active) {
        blips[i].active = 1; blips[i].pos = position; blips[i].type = blipType; blips[i].color = color & 3; return i; }
    return -1;
}
void RemoveRadarBlip(BlipID b) { if (b >= 0 && b < MAX_BLIPS) blips[b].active = 0; }
void DisplayObjectiveText(const char *t) { snprintf(objective_text, sizeof(objective_text), "%s", t); }
void SetTimerUI(float seconds, bool countUp) { mtimer.active = 1; mtimer.countup = countUp; mtimer.t = countUp ? 0.0f : seconds; }
bool IsPlayerInArea(Vector3 c, float r) { return dist2d(player.pos.x, player.pos.z, c.x, c.z) < r; }
bool IsPlayerInVehicle(VehicleID v) {
    int i = v - ENT_VEH_BASE; return i >= 0 && i < MAX_VEHICLES && vehicles[i].active && player.veh == i;
}
void SetPlayerWantedLevel(int stars) {
    stars = stars < 0 ? 0 : stars > 5 ? 5 : stars;
    wanted.heat = stars * HEAT_PER_STAR; wanted.unseen = 0.0f; heat_changed();
}
void SetPlayerOutfit(int outfitID) { player_outfit = outfitID < 0 ? 0 : outfitID > 3 ? 3 : outfitID; }
void ResprayVehicle(VehicleID v) {
    int i = v - ENT_VEH_BASE; if (i < 0 || i >= MAX_VEHICLES || !vehicles[i].active) return;
    uint32_t c; do c = RESPRAY_RGB[rnd() % 6]; while (c == vehicles[i].color_override);
    vehicles[i].color_override = c;
}
bool IsEntityDead(EntityID e) {
    if (e >= ENT_VEH_BASE && e < ENT_VEH_BASE + MAX_VEHICLES) return !vehicles[e - ENT_VEH_BASE].active;
    if (e >= 0 && e < MAX_NPCS) return !npcs[e].active || npcs[e].health <= 0;
    return true;                                 /* invalid ids count as dead */
}
void StartTailMeter(EntityID targetID, float minDistance, float maxDistance) {
    tail.active = 1; tail.target = targetID; tail.min_d = minDistance; tail.max_d = maxDistance; tail.value = 1.0f;
}
float GetTailMeterValue(void) { return tail.value; }          /* 1 = perfect tail, 0 = lost / spotted */
void TriggerPhotoCapture(void) { mphoto.armed = 1; mphoto.taken = 0; }

/* ---- Extra helpers the spec's API needed ---- */
void StopTailMeter(void) { tail.active = 0; }
void ClearTimerUI(void) { mtimer.active = 0; }
float GetTimerValue(void) { return mtimer.t; }
bool IsTimerExpired(void) { return mtimer.active && !mtimer.countup && mtimer.t <= 0.0f; }
Vector3 GetEntityPosition(EntityID e) { Vector3 p = {0, 0, 0}; ent_pos(e, &p); return p; }
bool IsPlayerInAnyVehicle(void) { return player.veh >= 0; }
void SetPhotoSubject(EntityID e) { mphoto.subject = e; }
bool PhotoTaken(void) { return mphoto.taken != 0; }
bool SetVehicleRoute(VehicleID v, const Vector3 *pts, int n, float speed) {
    int i = v - ENT_VEH_BASE;
    if (i < 0 || i >= MAX_VEHICLES || !vehicles[i].active || routes_used >= MAX_ROUTES || n < 1) return false;
    Route *r = &routes[routes_used]; r->n = n > ROUTE_PTS ? ROUTE_PTS : n;
    for (int k = 0; k < r->n; k++) r->p[k] = pts[k];
    vehicles[i].route_id = ++routes_used; vehicles[i].wp = 0; vehicles[i].cruise = speed; vehicles[i].ai = AI_ROUTE;
    return true;
}

/* ---- FSM control (called from mission update functions) ---- */
int  Mission_Step(void)        { return mission_step; }
bool Mission_StepEntered(void) { return mission_step_entered != 0; }   /* true on the first update of each step */
void Mission_StepDone(void)    { if (mission_state == MS_STEP_IN_PROGRESS) mission_state = MS_STEP_COMPLETE; }
void Mission_Succeed(void)     { if (mission_state == MS_STEP_IN_PROGRESS || mission_state == MS_STEP_COMPLETE) mission_state = MS_SUCCESS; }
void Mission_Fail(const char *why) {
    if (mission_state == MS_STEP_IN_PROGRESS || mission_state == MS_STEP_COMPLETE) {
        snprintf(fail_reason, sizeof(fail_reason), "%s", why); mission_state = MS_FAILED; }
}

static void photo_check_subject(void) {                     /* called when X is pressed in photo mode */
    if (!mphoto.armed || mphoto.taken) return;
    Vector3 sp;
    if (!ent_pos(mphoto.subject, &sp)) return;
    float dx = sp.x - photo_pos.x, dy = (sp.y + 1.2f) - photo_pos.y, dz = sp.z - photo_pos.z;
    float hd = sqrtf(dx*dx + dz*dz), dist = sqrtf(hd*hd + dy*dy);
    float dyaw = fabsf(wrap_pi(atan2f(dx, dz) - photo_yaw)), dpit = fabsf(atan2f(dy, hd) - photo_pitch);
    if (dist > 4.0f && dist < 120.0f && dyaw < 0.6f && dpit < 0.35f) { mphoto.taken = 1; banner("SUBJECT CAPTURED", 2.0f); }
    else banner("SUBJECT NOT IN FRAME", 2.0f);
}

static void mission_release_all(void) {
    for (int i = 0; i < MAX_BLIPS; i++) blips[i].active = 0;
    mtimer.active = 0; tail.active = 0; mphoto.armed = 0; mphoto.taken = 0; mphoto.subject = -1;
    player_outfit = 0; routes_used = 0; objective_text[0] = 0;
    for (int i = 0; i < MAX_NPCS; i++) npcs[i].pinned = 0;
    for (int i = 0; i < MAX_VEHICLES; i++) { vehicles[i].pinned = 0; if (vehicles[i].ai == AI_ROUTE) vehicles[i].ai = AI_NONE; }
}
static void mission_reset(void) {
    mission_active = -1; mission_state = MS_UNTRIGGERED; mission_step = 0; mission_step_entered = 0;
    banner_timer = 0.0f; memset(mission_done, 0, sizeof(mission_done)); mission_release_all();
}

/* ---- Save file (reward flags + cash), story mode only ---- */
#define SAVE_PATH "ms0:/PSP/GAME/LiwaSandbox/save.dat"
typedef struct { uint32_t magic, money, done_mask; } SaveData;
static void save_game(void) {
    SaveData d; d.magic = 0x4C495741u; d.money = player_money; d.done_mask = 0;
    for (int i = 0; i < MAX_MISSIONS; i++) if (mission_done[i]) d.done_mask |= 1u << i;
    SceUID fd = sceIoOpen(SAVE_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) { sceIoWrite(fd, &d, sizeof(d)); sceIoClose(fd); }
}
static void load_game(void) {
    SaveData d; SceUID fd = sceIoOpen(SAVE_PATH, PSP_O_RDONLY, 0); if (fd < 0) return;
    int n = sceIoRead(fd, &d, sizeof(d)); sceIoClose(fd);
    if (n != (int)sizeof(d) || d.magic != 0x4C495741u) return;
    player_money = d.money;
    for (int i = 0; i < MAX_MISSIONS; i++) mission_done[i] = (d.done_mask >> i) & 1u;
}

/* ====== PLACEHOLDER MISSIONS (replace with your story) ====================
 * Each mission = Start / Update / CleanUp + a row in MISSIONS[].
 * Update runs a switch(Mission_Step()) and calls Mission_StepDone(),
 * Mission_Succeed() or Mission_Fail(reason). Positions: x, y(ignored), z.
 * ======================================================================== */

/* M1 - TAIL: follow a Land Cruiser from the city out into the dunes. */
static VehicleID m1_car = -1, m1_target = -1; static BlipID m1_blip = -1;
static const Vector3 M1_ROUTE[] = { {-832,0,640}, {-704,0,640}, {-640,0,768}, {-420,0,560}, {-300,0,300}, {-120,0,150}, {150,0,-60} };
#define M1_ROUTE_N ((int)(sizeof(M1_ROUTE)/sizeof(M1_ROUTE[0])))
static void M1_Start(void) {
    Vector3 p = {-880, 0, 704}; m1_target = -1;
    m1_car = SpawnVehicle(VEH_Y61, p, 0.0f); m1_blip = AddRadarBlip(p, BLIPT_OBJECTIVE, BLIP_YELLOW);
    DisplayObjectiveText("Get in the Patrol");
}
static void M1_Update(float dt) {
    switch (Mission_Step()) {
    case 0:
        if (m1_car < 0) { Mission_Fail("No free vehicle slot"); break; }
        if (IsPlayerInVehicle(m1_car)) { RemoveRadarBlip(m1_blip); m1_blip = -1; Mission_StepDone(); }
        break;
    case 1:
        if (Mission_StepEntered()) {
            m1_target = SpawnVehicle(VEH_LC200, M1_ROUTE[0], atan2f(M1_ROUTE[1].x - M1_ROUTE[0].x, M1_ROUTE[1].z - M1_ROUTE[0].z));
            if (m1_target < 0 || !SetVehicleRoute(m1_target, M1_ROUTE, M1_ROUTE_N, 20.0f)) { Mission_Fail("Could not spawn target"); break; }
            StartTailMeter(m1_target, 25.0f, 90.0f);
            DisplayObjectiveText("Tail the Land Cruiser: stay 25-90 m back");
        }
        if (IsEntityDead(m1_target)) Mission_Fail("Target destroyed");
        else if (GetTailMeterValue() <= 0.0f) Mission_Fail("You lost them or were spotted");
        else {
            Vector3 tp = GetEntityPosition(m1_target);
            if (dist2d(tp.x, tp.z, M1_ROUTE[M1_ROUTE_N-1].x, M1_ROUTE[M1_ROUTE_N-1].z) < 20.0f) Mission_StepDone();
        }
        break;
    case 2: StopTailMeter(); DisplayObjectiveText("The target has reached the meeting point"); Mission_Succeed(); break;
    }
}
static void M1_CleanUp(void) { RemoveRadarBlip(m1_blip); if (m1_target >= 0) RemoveEntity(m1_target); }

/* M2 - PHOTO: photograph a wandering target (START = photo mode, X = shoot). */
static EntityID m2_target = -1; static BlipID m2_blip = -1;
static void M2_Start(void) {
    Vector3 p = {-960, 0, 960};
    m2_target = SpawnNPC(NPC_T_TARGET, p, 0.0f);
    m2_blip = AddRadarBlip(p, BLIPT_TARGET, BLIP_RED);
    SetPhotoSubject(m2_target); TriggerPhotoCapture(); SetTimerUI(120.0f, false);
    DisplayObjectiveText("Photograph the target. START = photo mode, X = shoot");
}
static void M2_Update(float dt) {
    if (m2_target < 0) { Mission_Fail("No free NPC slot"); return; }
    if (m2_blip >= 0) { blips[m2_blip].pos = GetEntityPosition(m2_target); }     /* blip follows target */
    if (IsEntityDead(m2_target)) Mission_Fail("Target killed");
    else if (IsTimerExpired()) Mission_Fail("Out of time");
    else if (PhotoTaken()) Mission_Succeed();
}
static void M2_CleanUp(void) { RemoveRadarBlip(m2_blip); if (m2_target >= 0) RemoveEntity(m2_target); }

/* M3 - RESPRAY: steal a car (2 stars), respray it at the garage, sell it in the dunes. */
static VehicleID m3_car = -1; static BlipID m3_blip = -1;
static const Vector3 M3_GARAGE = {-640, 0, 640}, M3_DROP = {-300, 0, 120};
static void M3_Start(void) {
    Vector3 p = {-1024, 0, 896};
    m3_car = SpawnVehicle(VEH_LC100, p, 0.0f); m3_blip = AddRadarBlip(p, BLIPT_OBJECTIVE, BLIP_YELLOW);
    SetPlayerOutfit(2); DisplayObjectiveText("Steal the Land Cruiser");
}
static void M3_Update(float dt) {
    if (Mission_Step() < 3 && IsEntityDead(m3_car)) { Mission_Fail("The vehicle was destroyed"); return; }
    switch (Mission_Step()) {
    case 0:
        if (IsPlayerInVehicle(m3_car)) {
            SetPlayerWantedLevel(2); RemoveRadarBlip(m3_blip); m3_blip = AddRadarBlip(M3_GARAGE, BLIPT_DESTINATION, BLIP_GREEN);
            DisplayObjectiveText("Lose the cops: get to the spray garage"); Mission_StepDone();
        }
        break;
    case 1:
        if (IsPlayerInVehicle(m3_car) && IsPlayerInArea(M3_GARAGE, 10.0f)) {
            ResprayVehicle(m3_car); SetPlayerWantedLevel(0); Mission_StepDone();
        }
        break;
    case 2:
        if (Mission_StepEntered()) {
            RemoveRadarBlip(m3_blip); m3_blip = AddRadarBlip(M3_DROP, BLIPT_DESTINATION, BLIP_GREEN);
            DisplayObjectiveText("Deliver the car to the buyer in the dunes");
        }
        if (IsPlayerInVehicle(m3_car) && IsPlayerInArea(M3_DROP, 15.0f)) Mission_Succeed();
        break;
    }
}
static void M3_CleanUp(void) { RemoveRadarBlip(m3_blip); SetPlayerOutfit(0); }

static const MissionDef MISSIONS[MAX_MISSIONS] = {
    { "Tail the Cruiser", {-896, 0, 704},  4000, M1_Start, M1_Update, M1_CleanUp },
    { "Paparazzi",        {-1024, 0, 832}, 3000, M2_Start, M2_Update, M2_CleanUp },
    { "Respray Job",      {-832, 0, 768},  6000, M3_Start, M3_Update, M3_CleanUp },
};

/* ---- Framework tick ---- */
static void mission_begin(int i) {
    mission_release_all();
    mission_active = i; mission_state = MS_INIT; mission_step = 0;
    MISSIONS[i].start();                                    /* Mission_Start(): spawn, blips, first objective */
    mission_state = MS_STEP_IN_PROGRESS; mission_step_entered = 1;
}
static void Missions_Tick(float dt) {
    if (banner_timer > 0.0f) banner_timer -= dt;
    if (mission_active < 0) {
        if (resp_state != RESP_NONE || player.veh >= 0 || wanted.wanted_stars > 0) return;
        for (int i = 0; i < MAX_MISSIONS; i++)
            if (!mission_done[i] && dist2d(player.pos.x, player.pos.z, MISSIONS[i].trigger.x, MISSIONS[i].trigger.z) < 4.0f) { mission_begin(i); break; }
        return;
    }
    const MissionDef *m = &MISSIONS[mission_active];
    if (mtimer.active) { if (mtimer.countup) mtimer.t += dt; else if (mtimer.t > 0.0f) mtimer.t -= dt; }
    if (tail.active) {
        Vector3 tp;
        if (ent_pos(tail.target, &tp)) {
            float d = dist2d(player.pos.x, player.pos.z, tp.x, tp.z);
            if (d < tail.min_d) tail.value -= 0.20f * dt;            /* too close: they notice you */
            else if (d > tail.max_d) tail.value -= 0.12f * dt;       /* too far: losing them */
            else tail.value += 0.08f * dt;
            tail.value = clampf(tail.value, 0.0f, 1.0f);
        }
    }
    if (resp_state != RESP_NONE) Mission_Fail(resp_state == RESP_WASTED ? "Wasted" : "Busted");

    switch (mission_state) {
    case MS_STEP_IN_PROGRESS: m->update(dt); mission_step_entered = 0; break;     /* Mission_Update() */
    case MS_STEP_COMPLETE:    mission_step++; mission_step_entered = 1; mission_state = MS_STEP_IN_PROGRESS; break;
    case MS_SUCCESS: case MS_FAILED: {
        int ok = (mission_state == MS_SUCCESS), idx = mission_active;
        m->cleanup();                                                              /* Mission_CleanUp() */
        mission_release_all();
        if (ok) {
            player_money += m->reward; mission_done[idx] = 1;
            char t[64]; snprintf(t, sizeof(t), "MISSION PASSED  +$%u", (unsigned)m->reward); banner(t, 4.0f);
            if (story_mode) save_game();
        } else { char t[64]; snprintf(t, sizeof(t), "MISSION FAILED: %s", fail_reason); banner(t, 4.0f); }
        mission_active = -1; mission_state = MS_UNTRIGGERED; mission_step = 0;
        break; }
    default: break;
    }
}

/* ------------------------------------------------------------------------ */
/*  Reset / init                                                             */
/* ------------------------------------------------------------------------ */
static void controls_defaults(void) {
    ctl.accelerate = PSP_CTRL_CROSS; ctl.brake = PSP_CTRL_CIRCLE;
    ctl.handbrake = PSP_CTRL_SQUARE; ctl.interact = PSP_CTRL_TRIANGLE;
}
static void reset_world(int story) {
    memset(vehicles, 0, sizeof(vehicles)); memset(npcs, 0, sizeof(npcs));
    memset(&wanted, 0, sizeof(wanted)); memset(&race, 0, sizeof(race)); memset(chunks, 0, sizeof(chunks));
    memset(&player, 0, sizeof(player));
    player.pos.x = -960.0f; player.pos.z = 768.0f; player.pos.y = 0.0f; player.health = 100;
    player.veh = -1; player.owned[0] = 1; player.on_ground = 1;
    player_money = story ? 5000u : 25000u; bank_cooldown = 0.0f; resp_state = RESP_NONE;
    build_statics();
    for (int i = 0; i < MAX_RACE_CKPT; i++) {
        race_cp[i].position.x = RACE_PTS[i][0]; race_cp[i].position.z = RACE_PTS[i][1];
        race_cp[i].position.y = terrain_height(RACE_PTS[i][0], RACE_PTS[i][1]); race_cp[i].is_triggered = 0;
    }
    update_chunks(player.pos.x, player.pos.z);
    /* city fleet (on 64 m road lines) */
    spawn_vehicle(VEH_LC100,    -960.0f, 720.0f, 0.0f, AI_NONE);
    spawn_vehicle(VEH_Y60,      -896.0f, 768.0f, PI_F*0.5f, AI_NONE);
    spawn_vehicle(VEH_Y62,     -1024.0f, 704.0f, 0.0f, AI_NONE);
    spawn_vehicle(VEH_DIRT_BIKE, -960.0f, 750.0f, 0.0f, AI_NONE);
    spawn_vehicle(VEH_QUAD,     -880.0f, 640.0f, 0.0f, AI_NONE);
    /* desert + airport */
    spawn_vehicle(VEH_Y61,  -500.0f, 400.0f, 0.5f, AI_NONE);
    spawn_vehicle(VEH_QUAD, -440.0f, 300.0f, 0.0f, AI_NONE);
    spawn_vehicle(VEH_DIRT_BIKE, -460.0f, 360.0f, 0.0f, AI_NONE);
    spawn_vehicle(VEH_PLANE, 720.0f, -1000.0f, PI_F*0.5f, AI_NONE);
    for (int i = 0; i < MAX_VEHICLES; i++) if (vehicles[i].active) vehicles[i].owned = 0;
    mission_reset();
    cam_yaw = player.yaw; status_timer = 0;
    look_yaw = look_pitch = aim_pitch = 0.0f; fp_mode = lock_active = cam_snap = 0; lock_idx = -1;
}
static void enter_gameplay(int story) { story_mode = story; reset_world(story); if (story) load_game(); current_state = STATE_GAMEPLAY; }

/* ------------------------------------------------------------------------ */
/*  Gameplay update                                                          */
/* ------------------------------------------------------------------------ */
static void update_gameplay(float dt, const SceCtrlData *pad, uint32_t pressed) {
    game_time += dt;
    float lx = axis(pad->Lx), ly = axis(pad->Ly);
    uint32_t b = pad->Buttons;
    int lheld = (b & PSP_CTRL_LTRIGGER) != 0, rheld = (b & PSP_CTRL_RTRIGGER) != 0;
    float ext_l = (b & PSP_CTRL_LEFT) ? 1.0f : 0.0f, ext_r = (b & PSP_CTRL_RIGHT) ? 1.0f : 0.0f;   /* plane rudder */
    if (fire_cd > 0) fire_cd -= dt;
    if (fire_alert > 0) fire_alert -= dt;
    if (status_timer > 0) status_timer -= dt;
    if (bank_cooldown > 0) bank_cooldown -= dt;

    if (resp_state != RESP_NONE) {
        resp_timer -= dt; lx = ly = 0; b = 0; pressed = 0; lheld = rheld = 0; ext_l = ext_r = 0;
        if (resp_timer <= 0.0f) do_respawn();
    } else {
        if (pressed & PSP_CTRL_START) {
            photo_pos = cam_eye; photo_yaw = cam_yaw + look_yaw; photo_pitch = -0.15f; photo_hide_ui = 0;
            fp_mode = 0; lock_active = 0; current_state = STATE_PHOTO_MODE; return;
        }
        if (pressed & PSP_CTRL_SELECT) {                          /* camera distance cycle */
            static const char *FN[3] = { "Close", "Medium", "Far" }, *VN[4] = { "Close", "Medium", "Far", "Bumper" };
            if (player.veh >= 0) { cam_mode_veh = (cam_mode_veh + 1) % 4; set_status("Camera: %s", VN[cam_mode_veh]); }
            else { cam_mode_foot = (cam_mode_foot + 1) % 3; set_status("Camera: %s", FN[cam_mode_foot]); }
        }
    }
    if (pressed & PSP_CTRL_LTRIGGER) l_hold_t = 0.0f;
    if (lheld) l_hold_t += dt;
    if ((g_released & PSP_CTRL_LTRIGGER) && l_hold_t < 0.25f) cam_snap = 1;      /* tap L: snap behind */

    /* Single nub: movement OR camera. */
    float mlx = lx, mly = ly; fp_mode = 0;
    if (player.veh < 0 && resp_state == RESP_NONE) {
        fp_mode = (player.weapon == 3 && rheld);                  /* sniper + R = first-person aim */
        if (fp_mode) {
            player.yaw -= lx * 1.6f * dt; aim_pitch = clampf(aim_pitch - ly * 1.0f * dt, -0.7f, 0.7f);
            mlx = mly = 0.0f;
        } else if (lheld) mlx = mly = 0.0f;                       /* stationary while free-looking */
    }
    float vlx = lx, vly = ly; uint32_t vb = b;
    if (player.veh >= 0 && lheld) {                               /* in a car: stick looks, circle = drive-by */
        vlx = vly = 0.0f;
        int side = lx < -0.5f ? 1 : (lx > 0.5f ? -1 : 0);        /* +1 = driver (left) side */
        if (side && player.weapon > 0 && (b & ctl.brake)) {
            vb &= ~ctl.brake;
            if (fire_cd <= 0.0f && player.ammo[player.weapon] > 0) fire_weapon(vehicles[player.veh].yaw + side * PI_F * 0.5f);
        }
    }

    float tx, tz;                                                 /* lock-on target selection */
    lock_active = 0;
    if (player.veh < 0 && rheld && !fp_mode && resp_state == RESP_NONE) {
        if ((pressed & PSP_CTRL_RTRIGGER) || !lock_pos(lock_idx, &tx, &tz)) lock_idx = lock_nearest();
        lock_active = lock_idx >= 0;
    } else lock_idx = -1;

    if (player.veh < 0 && resp_state == RESP_NONE) player_on_foot(mlx, mly, b, pressed, dt, rheld);
    update_vehicles(dt, vlx, vly, vb, pressed, ext_l, ext_r);
    update_npcs(dt); update_wanted(dt); update_race(dt); Missions_Tick(dt);
    update_chunks(player.pos.x, player.pos.z);

    if (resp_state == RESP_NONE) {
        if (player.health <= 0) begin_resp(RESP_WASTED);
        else if (wanted.bust > 2.5f) begin_resp(RESP_BUSTED);
    }
    cam_fov = fp_mode ? 28.0f : 60.0f;
    camera_follow(dt, lx, ly, lheld);
}

/* ------------------------------------------------------------------------ */
/*  Interior / shops / heist                                                 */
/* ------------------------------------------------------------------------ */
static const int SHOP_DEALER_TYPES[2] = { VEH_Y61, VEH_LC200 };
static void update_interior(float dt, uint32_t held, uint32_t pressed) {
    if (status_timer > 0) status_timer -= dt;
    if (bank_cooldown > 0) bank_cooldown -= dt;
    if (pressed & PSP_CTRL_CIRCLE) { current_state = STATE_GAMEPLAY; return; }
    int n = (store_type == STORE_DEALERSHIP) ? 2 : (store_type == STORE_GUNSHOP) ? 6 : 0;
    if (n) {
        if (pressed & PSP_CTRL_UP)   store_sel = (store_sel + n - 1) % n;
        if (pressed & PSP_CTRL_DOWN) store_sel = (store_sel + 1) % n;
    }
    if (store_type == STORE_DEALERSHIP && (pressed & PSP_CTRL_CROSS)) {
        int t = SHOP_DEALER_TYPES[store_sel], price = VSPEC[t].price;
        if (player_money < (uint32_t)price) set_status("Not enough cash");
        else {
            int i = spawn_vehicle(t, POIS[store_poi].x, POIS[store_poi].z - 32.0f, PI_F*0.5f, AI_NONE);
            if (i < 0) set_status("No free vehicle slot");
            else { vehicles[i].owned = 1; player_money -= (uint32_t)price; set_status("%s delivered outside", VSPEC[t].name); }
        }
    }
    if (store_type == STORE_GUNSHOP && (pressed & PSP_CTRL_CROSS)) {
        static const int price[6] = {500, 4000, 7500, 150, 400, 300}, pack[3] = {24, 60, 10};
        int wi = (store_sel < 3) ? store_sel + 1 : store_sel - 2;     /* weapon index 1..3 */
        if (player_money < (uint32_t)price[store_sel]) set_status("Not enough cash");
        else if (store_sel < 3 && player.owned[wi]) set_status("You already own the %s", WNAME[wi]);
        else if (store_sel >= 3 && !player.owned[wi]) set_status("Buy the %s first", WNAME[wi]);
        else {
            player_money -= (uint32_t)price[store_sel];
            player.owned[wi] = 1; player.ammo[wi] += pack[wi - 1];
            set_status("Purchased");
        }
    }
    if (store_type == STORE_BANK_VAULT) {
        if (bank_cooldown > 0.0f) heist_prog = 0.0f;
        else if (held & PSP_CTRL_CROSS) heist_prog += dt / 3.0f;
        else heist_prog = fmaxf(0.0f, heist_prog - dt * 0.5f);
        if (heist_prog >= 1.0f) {
            uint32_t loot = 9000u + (rnd() % 6000u);
            player_money += loot; bank_cooldown = 180.0f; heist_prog = 0.0f;
            add_heat(230.0f);                                    /* alarm: at least 2 stars */
            set_status("Vault cracked! +$%u - police alerted", (unsigned)loot);
            current_state = STATE_GAMEPLAY;
        }
    }
}

/* ------------------------------------------------------------------------ */
/*  Photo mode (world paused, rendering continues)                           */
/* ------------------------------------------------------------------------ */
static void update_photo(float dt, const SceCtrlData *pad, uint32_t pressed) {
    float lx = axis(pad->Lx), ly = axis(pad->Ly), sp = (pad->Buttons & PSP_CTRL_SQUARE) ? 120.0f : 35.0f;
    if (pad->Buttons & PSP_CTRL_LEFT)  photo_yaw   += 1.4f*dt;
    if (pad->Buttons & PSP_CTRL_RIGHT) photo_yaw   -= 1.4f*dt;
    if (pad->Buttons & PSP_CTRL_UP)    photo_pitch += 1.0f*dt;
    if (pad->Buttons & PSP_CTRL_DOWN)  photo_pitch -= 1.0f*dt;
    photo_pitch = clampf(photo_pitch, -1.4f, 1.4f);
    float cp = cosf(photo_pitch);
    float fx = sinf(photo_yaw)*cp, fy = sinf(photo_pitch), fz = cosf(photo_yaw)*cp;
    float rx = -cosf(photo_yaw), rz = sinf(photo_yaw);
    photo_pos.x += (fx*(-ly) + rx*lx) * sp * dt; photo_pos.y += fy*(-ly) * sp * dt; photo_pos.z += (fz*(-ly) + rz*lx) * sp * dt;
    if (pad->Buttons & PSP_CTRL_RTRIGGER) photo_pos.y += sp*dt;
    if (pad->Buttons & PSP_CTRL_LTRIGGER) photo_pos.y -= sp*dt;
    float g = terrain_height(photo_pos.x, photo_pos.z) + 1.0f; if (photo_pos.y < g) photo_pos.y = g;
    if (pressed & PSP_CTRL_CROSS) { capture_req = 1; photo_check_subject(); }
    if (pressed & PSP_CTRL_TRIANGLE) photo_hide_ui = !photo_hide_ui;
    if (pressed & PSP_CTRL_START) current_state = STATE_GAMEPLAY;
    if (status_timer > 0) status_timer -= dt;
}

/* ------------------------------------------------------------------------ */
/*  Gallery + controls config                                                */
/* ------------------------------------------------------------------------ */
static void gallery_scan(void) {
    gallery_count = 0; gallery_sel = 0;
    SceUID d = sceIoDopen(PHOTO_DIR); if (d < 0) return;
    SceIoDirent e; memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0 && gallery_count < MAX_GALLERY) {
        if (FIO_S_ISREG(e.d_stat.st_mode)) {
            strncpy(gallery[gallery_count].name, e.d_name, 31); gallery[gallery_count].name[31] = 0;
            gallery[gallery_count].size = (int)e.d_stat.st_size; gallery_count++;
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
}
static void update_gallery(uint32_t pressed) {
    if (pressed & PSP_CTRL_UP)   gallery_sel = gallery_sel > 0 ? gallery_sel - 1 : 0;
    if (pressed & PSP_CTRL_DOWN) gallery_sel = gallery_sel + 1 < gallery_count ? gallery_sel + 1 : gallery_sel;
    if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) current_state = STATE_MAIN_MENU;
}
static uint32_t *ctl_slot(int row) {
    return row == 0 ? &ctl.accelerate : row == 1 ? &ctl.brake : row == 2 ? &ctl.handbrake : &ctl.interact;
}
static const char *btn_name(uint32_t m) {
    return m == PSP_CTRL_CROSS ? "CROSS" : m == PSP_CTRL_CIRCLE ? "CIRCLE" : m == PSP_CTRL_SQUARE ? "SQUARE" : m == PSP_CTRL_TRIANGLE ? "TRIANGLE" : "?";
}
static void update_controls(uint32_t pressed) {
    if (ctl_listen) {
        static const uint32_t faces[4] = { PSP_CTRL_CROSS, PSP_CTRL_CIRCLE, PSP_CTRL_SQUARE, PSP_CTRL_TRIANGLE };
        for (int i = 0; i < 4; i++) if (pressed & faces[i]) {
            uint32_t *mine = ctl_slot(ctl_row), old = *mine;
            for (int r = 0; r < 4; r++) if (r != ctl_row && *ctl_slot(r) == faces[i]) *ctl_slot(r) = old;   /* swap duplicates */
            *mine = faces[i]; ctl_listen = 0; break;
        }
        return;
    }
    if (pressed & PSP_CTRL_UP)   ctl_row = (ctl_row + 3) % 4;
    if (pressed & PSP_CTRL_DOWN) ctl_row = (ctl_row + 1) % 4;
    if (pressed & PSP_CTRL_CROSS) ctl_listen = 1;
    if (pressed & PSP_CTRL_SELECT) controls_defaults();
    if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) current_state = STATE_MAIN_MENU;
}

/* ------------------------------------------------------------------------ */
/*  Rendering: GU helpers                                                    */
/* ------------------------------------------------------------------------ */
#define VTYPE (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

static void model_identity(void) { sceGumMatrixMode(GU_MODEL); sceGumLoadIdentity(); }

/* Axis-aligned box centred at (cx,cy,cz) in the CURRENT model space. 36 verts
 * are carved out of the display list (no malloc); faces are pre-shaded. */
static void draw_box(float cx, float cy, float cz, float sx, float sy, float sz, uint32_t color) {
    static const unsigned char F[6][4] = { {4,5,6,7}, {1,0,3,2}, {5,1,2,6}, {0,4,7,3}, {3,7,6,2}, {0,1,5,4} };
    static const float SH[6] = { 0.85f, 0.70f, 0.78f, 0.62f, 1.00f, 0.50f };   /* +z -z +x -x top bottom */
    float x0 = cx - sx*0.5f, x1 = cx + sx*0.5f, y0 = cy - sy*0.5f, y1 = cy + sy*0.5f, z0 = cz - sz*0.5f, z1 = cz + sz*0.5f;
    float C[8][3] = { {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1} };
    CVertex *v = (CVertex *)sceGuGetMemory(36 * sizeof(CVertex)); int n = 0;
    for (int f = 0; f < 6; f++) {
        uint32_t c = shade(color, SH[f]); static const int q[6] = {0,1,2,0,2,3};
        for (int k = 0; k < 6; k++) { int ci = F[f][q[k]]; v[n].c = c; v[n].x = C[ci][0]; v[n].y = C[ci][1]; v[n].z = C[ci][2]; n++; }
    }
    sceGumDrawArray(GU_TRIANGLES, VTYPE, 36, 0, v);
}

/* Terrain: cached height grid, rebuilt only when the camera crosses a cell. */
#define TG_N    36
#define TG_CELL 20.0f
static float    tg_h[TG_N+1][TG_N+1];
static uint32_t tg_c[TG_N+1][TG_N+1];
static int      tg_ox = 0x7FFFFFFF, tg_oz = 0x7FFFFFFF;

static uint32_t terrain_color(float x, float z, float h, float f) {
    float t = clampf(0.5f + h/28.0f, 0.0f, 1.0f);
    float sr = 190 + t*50, sg = 150 + t*55, sb = 95 + t*55, fr, fg, fb;
    int zn = zone_at(x, z);
    if (zn == ZONE_AIRPORT) { fr = 88; fg = 88; fb = 92; } else if (zn == ZONE_CITY) { fr = 125; fg = 120; fb = 115; } else { fr = 165; fg = 155; fb = 135; }
    return RGB((int)(sr + (fr - sr)*f), (int)(sg + (fg - sg)*f), (int)(sb + (fb - sb)*f));
}
static void draw_terrain(float camx, float camz) {
    int ox = (int)floorf(camx / TG_CELL) - TG_N/2, oz = (int)floorf(camz / TG_CELL) - TG_N/2;
    if (ox != tg_ox || oz != tg_oz) {
        tg_ox = ox; tg_oz = oz;
        for (int j = 0; j <= TG_N; j++) for (int i = 0; i <= TG_N; i++) {
            float wx = (ox + i) * TG_CELL, wz = (oz + j) * TG_CELL, f = flatness(wx, wz);
            tg_h[j][i] = dune_h(wx, wz) * (1.0f - f); tg_c[j][i] = terrain_color(wx, wz, tg_h[j][i], f);
        }
    }
    model_identity();
    for (int j = 0; j < TG_N; j++) {
        CVertex *v = (CVertex *)sceGuGetMemory(2 * (TG_N+1) * sizeof(CVertex));
        for (int i = 0; i <= TG_N; i++) {
            float wx = (ox + i) * TG_CELL, wz0 = (oz + j) * TG_CELL, wz1 = wz0 + TG_CELL;
            v[i*2].c   = tg_c[j][i];   v[i*2].x   = wx; v[i*2].y   = tg_h[j][i];   v[i*2].z   = wz0;
            v[i*2+1].c = tg_c[j+1][i]; v[i*2+1].x = wx; v[i*2+1].y = tg_h[j+1][i]; v[i*2+1].z = wz1;
        }
        sceGumDrawArray(GU_TRIANGLE_STRIP, VTYPE, 2 * (TG_N+1), 0, v);
    }
}

static int visible(float x, float z, float ex, float ez, float range) { return dist2d(x, z, ex, ez) < range; }
static void translate3(float x, float y, float z) { ScePspFVector3 t = { x, y, z }; sceGumTranslate(&t); }

static void draw_vehicle(const Vehicle *v) {
    const VehSpec *s = &VSPEC[v->type];
    sceGumMatrixMode(GU_MODEL); sceGumLoadIdentity();
    translate3(v->pos.x, v->pos.y, v->pos.z);
    sceGumRotateY(v->yaw); sceGumRotateX(-v->pitch);
    sceGumRotateZ(v->flip_timer > 0 ? PI_F*0.9f : (v->roll + v->lean));
    float hw = s->hw, hl = s->hl; uint32_t c = v->color_override ? v->color_override : s->color, dark = shade(c, 0.45f);
    switch (s->cls) {
    case CLS_CAR:
        draw_box(0, 0.95f, 0, hw*2, 0.9f, hl*2, c);
        draw_box(0, 1.75f, -hl*0.1f, hw*1.8f, 0.8f, hl*1.1f, dark);
        for (int i = 0; i < 4; i++) draw_box((i&1 ? hw : -hw), 0.45f, (i&2 ? hl*0.65f : -hl*0.65f), 0.35f, 0.9f, 0.9f, RGB(25,25,25));
        if (v->type >= VEH_POLICE_SEDAN) {
            int ph = ((int)(game_time * 6.0f)) & 1;
            draw_box(-hw*0.4f, 2.25f, 0, hw*0.7f, 0.25f, 0.5f, ph ? RGB(255,30,30) : RGB(60,0,0));
            draw_box( hw*0.4f, 2.25f, 0, hw*0.7f, 0.25f, 0.5f, ph ? RGB(0,0,60) : RGB(40,80,255));
        }
        break;
    case CLS_BIKE:
        draw_box(0, 0.7f, 0, 0.3f, 0.5f, hl*1.4f, c);
        draw_box(0, 0.4f, hl*0.85f, 0.15f, 0.8f, 0.5f, RGB(25,25,25));
        draw_box(0, 0.4f, -hl*0.85f, 0.15f, 0.8f, 0.5f, RGB(25,25,25));
        draw_box(0, 1.15f, 0.3f, 0.75f, 0.1f, 0.15f, RGB(40,40,40));
        break;
    case CLS_QUAD:
        draw_box(0, 0.8f, 0, hw*1.4f, 0.5f, hl*1.6f, c);
        for (int i = 0; i < 4; i++) draw_box((i&1 ? hw : -hw), 0.4f, (i&2 ? hl*0.65f : -hl*0.65f), 0.5f, 0.8f, 0.8f, RGB(25,25,25));
        draw_box(0, 1.2f, 0.3f, hw*1.2f, 0.12f, 0.2f, RGB(40,40,40));
        break;
    case CLS_AIR:
        draw_box(0, 1.4f, 0, 1.4f, 1.4f, hl*2, c);
        draw_box(0, 1.6f, 0.4f, hw*2, 0.15f, 1.7f, shade(c, 0.85f));
        draw_box(0, 2.2f, -hl*0.9f, 0.15f, 1.3f, 1.2f, RGB(200,40,40));
        draw_box(0, 1.5f, -hl*0.95f, 3.6f, 0.12f, 1.0f, shade(c, 0.85f));
        draw_box(0, 1.4f, hl + 0.05f, 0.1f, 1.5f, 0.1f, RGB(60,60,60));
        break;
    }
}

static void draw_world(const Vector3D *eye, const Vector3D *ctr) {
    ScePspFVector3 e = { eye->x, eye->y, eye->z }, c = { ctr->x, ctr->y, ctr->z }, up = { 0, 1, 0 };
    sceGumMatrixMode(GU_VIEW); sceGumLoadIdentity(); sceGumLookAt(&e, &c, &up);
    draw_terrain(eye->x, eye->z);
    model_identity();

    /* runway + stripes */
    draw_box(1000, 0.15f, -1000, 600, 0.3f, 30, RGB(45,45,50));
    for (int i = 0; i < 15; i++) draw_box(730.0f + i*40.0f, 0.35f, -1000, 14, 0.1f, 1.0f, RGB(240,240,240));

    /* POI buildings + beacons + static airport solids */
    for (int i = 0; i < NUM_POIS; i++) {
        if (!visible(POIS[i].x, POIS[i].z, eye->x, eye->z, 720.0f)) continue;
        if (POIS[i].hw > 0) {
            draw_box(POIS[i].x, POIS[i].h*0.5f, POIS[i].z, POIS[i].hw*2, POIS[i].h, POIS[i].hd*2, POIS[i].color);
            draw_box(POIS[i].x, POIS[i].h + 1.0f, POIS[i].z, POIS[i].hw*1.4f, 2.0f, POIS[i].hd*1.4f, shade(POIS[i].color, 0.7f));
        }
        float gy = terrain_height(POIS[i].x, POIS[i].z);
        draw_box(POIS[i].x, gy + 45.0f, POIS[i].z, 1.5f, 90.0f, 1.5f, POIS[i].color);
    }
    for (int i = 0; i < MAX_MISSIONS; i++)                          /* mission start markers */
        if (mission_active < 0 && !mission_done[i] && visible(MISSIONS[i].trigger.x, MISSIONS[i].trigger.z, eye->x, eye->z, 720.0f))
            draw_box(MISSIONS[i].trigger.x, terrain_height(MISSIONS[i].trigger.x, MISSIONS[i].trigger.z) + 30.0f, MISSIONS[i].trigger.z, 2.0f, 60.0f, 2.0f, RGB(255,200,0));
    for (int i = 0; i < MAX_BLIPS; i++) if (blips[i].active) {         /* radar blips as world beacons */
        float gy = terrain_height(blips[i].pos.x, blips[i].pos.z);
        draw_box(blips[i].pos.x, gy + 35.0f, blips[i].pos.z, 1.2f, 70.0f, 1.2f, BLIP_RGB[blips[i].color]);
        if (blips[i].type == BLIPT_TARGET) draw_box(blips[i].pos.x, gy + 10.0f, blips[i].pos.z, 2.5f, 2.5f, 2.5f, BLIP_RGB[blips[i].color]);
    }
    for (int i = 0; i < 3; i++) draw_box(850.0f + i*100.0f, 7, -1085, 60, 14, 50, RGB(150,155,160));
    draw_box(1150, 15, -1075, 10, 30, 10, RGB(200,200,205));

    /* streamed chunk props */
    for (int cI = 0; cI < MAX_CHUNKS; cI++) if (chunks[cI].loaded)
        for (int i = 0; i < chunks[cI].nprops; i++) {
            const Prop *p = &chunks[cI].props[i];
            if (!visible(p->x, p->z, eye->x, eye->z, 340.0f)) continue;
            draw_box(p->x, p->y + p->h*0.5f, p->z, p->w, p->h, p->d, p->color);
            if (p->kind == 1) draw_box(p->x, p->y + p->h + 0.3f, p->z, 6.0f, 0.6f, 6.0f, RGB(40,130,50));
        }

    /* race markers */
    if (race.active) for (int i = race.next; i < MAX_RACE_CKPT; i++)
        draw_box(race_cp[i].position.x, race_cp[i].position.y + 25.0f, race_cp[i].position.z, i == race.next ? 4.0f : 1.5f, 50.0f, i == race.next ? 4.0f : 1.5f,
                 i == race.next ? RGB(255,220,40) : RGB(120,110,60));

    for (int i = 0; i < MAX_VEHICLES; i++) if (vehicles[i].active && visible(vehicles[i].pos.x, vehicles[i].pos.z, eye->x, eye->z, 400.0f)) draw_vehicle(&vehicles[i]);
    model_identity();
    for (int i = 0; i < MAX_NPCS; i++) if (npcs[i].active) {
        draw_box(npcs[i].pos.x, npcs[i].pos.y + 0.85f, npcs[i].pos.z, 0.6f, 1.2f, 0.4f, npcs[i].color);
        draw_box(npcs[i].pos.x, npcs[i].pos.y + 1.6f, npcs[i].pos.z, 0.35f, 0.35f, 0.35f, RGB(220,180,140));
    }
    if (lock_active && current_state == STATE_GAMEPLAY) {                /* lock-on marker */
        float lx2, lz2;
        if (lock_pos(lock_idx, &lx2, &lz2))
            draw_box(lx2, terrain_height(lx2, lz2) + 3.4f, lz2, 0.6f, 0.6f, 0.6f, (((int)(game_time*8.0f)) & 1) ? RGB(255,40,40) : RGB(255,170,40));
    }
    if (player.veh < 0 && current_state != STATE_MAIN_MENU && !fp_mode) {
        draw_box(player.pos.x, player.pos.y + 0.85f, player.pos.z, 0.65f, 1.2f, 0.45f, OUTFIT_RGB[player_outfit]);
        draw_box(player.pos.x, player.pos.y + 1.6f, player.pos.z, 0.38f, 0.38f, 0.38f, RGB(220,180,140));
    }
}

/* ------------------------------------------------------------------------ */
/*  Graphics init + frame                                                    */
/* ------------------------------------------------------------------------ */
static void init_graphics(void) {
    sceGuInit(); sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_5650, (void *)0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)FRAME_SIZE, BUF_W);
    sceGuDepthBuffer((void *)DEPTH_OFF, BUF_W);
    sceGuOffset(2048 - (SCR_W/2), 2048 - (SCR_H/2));
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_W, SCR_H); sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL); sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CW); sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH); sceGuEnable(GU_CLIP_PLANES);
    sceGuFog(80.0f, 340.0f, SKY_COLOR);
    sceGuFinish(); sceGuSync(0, 0);
    sceDisplayWaitVblankStart(); sceGuDisplay(GU_TRUE);
}

static void render_frame(void) {
    sceGuStart(GU_DIRECT, list);
    int world = (current_state == STATE_MAIN_MENU || current_state == STATE_GAMEPLAY || current_state == STATE_PHOTO_MODE);
    sceGuClearColor(world ? SKY_COLOR : 0xFF2A2018); sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    if (world) {
        sceGuEnable(GU_FOG);
        sceGumMatrixMode(GU_PROJECTION); sceGumLoadIdentity(); sceGumPerspective((current_state == STATE_GAMEPLAY) ? cam_fov : 60.0f, (float)SCR_W/(float)SCR_H, 1.0f, 1200.0f);
        Vector3D eye, ctr;
        if (current_state == STATE_MAIN_MENU) {
            float a = game_time * 0.15f;
            eye.x = CITY_CX + sinf(a)*220.0f; eye.y = 90.0f; eye.z = CITY_CZ + cosf(a)*220.0f; ctr.x = CITY_CX; ctr.y = 15.0f; ctr.z = CITY_CZ;
        } else if (current_state == STATE_PHOTO_MODE) {
            float cp = cosf(photo_pitch); eye = photo_pos;
            ctr.x = eye.x + sinf(photo_yaw)*cp; ctr.y = eye.y + sinf(photo_pitch); ctr.z = eye.z + cosf(photo_yaw)*cp;
        } else { eye = cam_eye; ctr = cam_ctr; }
        draw_world(&eye, &ctr);
    } else sceGuDisable(GU_FOG);
    sceKernelDcacheWritebackAll();
    sceGuFinish(); sceGuSync(0, 0);
}

/* ------------------------------------------------------------------------ */
/*  CPU-side HUD text (draws straight into the finished draw buffer)         */
/* ------------------------------------------------------------------------ */
static uint16_t *fb(void) { return (uint16_t *)(VRAM_UNCACHED + draw_off); }
static void put_pixel(int x, int y, uint16_t c) { if ((unsigned)x < SCR_W && (unsigned)y < SCR_H) fb()[y*BUF_W + x] = c; }
static void put_rect(int x, int y, int w, int h, uint16_t c) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) put_pixel(x+i, y+j, c); }
static void put_char(int x, int y, char ch, uint16_t col) {
    const unsigned char *g = &msx[(unsigned char)ch * 8];
    for (int j = 0; j < 8; j++) for (int i = 0; i < 8; i++) if (g[j] & (0x80 >> i)) put_pixel(x+i, y+j, col);
}
static void text(int x, int y, uint16_t col, const char *fmt, ...) {
    char buf[80]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    for (int k = 0; buf[k]; k++) put_char(x + k*8 + 1, y + 1, buf[k], 0x0000);
    for (int k = 0; buf[k]; k++) put_char(x + k*8, y, buf[k], col);
}
#define C_WHITE  0xFFFF
#define C_YELLOW RGB565(255,220,40)
#define C_RED    RGB565(255,70,60)
#define C_GREEN  RGB565(90,255,120)
#define C_GREY   RGB565(170,170,170)

static void draw_mission_hud(void) {
    float tx = 0, tz = 0; int have = 0;
    if (mission_active >= 0) {
        text(170, 6, C_GREY, "%s", MISSIONS[mission_active].name);
        text(20, 196, C_YELLOW, "%s", objective_text);
        if (mtimer.active) { int sec = (int)mtimer.t; text(212, 18, mtimer.t < 15.0f && !mtimer.countup ? C_RED : C_WHITE, "%02d:%02d", sec / 60, sec % 60); }
        if (tail.active) {
            Vector3 tp; text(150, 30, C_WHITE, "TAIL");
            put_rect(185, 31, 120, 6, RGB565(60,60,60)); put_rect(185, 31, (int)(tail.value * 120.0f), 6, tail.value > 0.4f ? RGB565(80,220,90) : RGB565(240,60,50));
            if (ent_pos(tail.target, &tp)) {
                float d = dist2d(player.pos.x, player.pos.z, tp.x, tp.z);
                if (d < tail.min_d) text(312, 30, C_RED, "TOO CLOSE"); else if (d > tail.max_d) text(312, 30, C_RED, "TOO FAR");
            }
        }
        if (mphoto.armed && !mphoto.taken) text(20, 208, C_GREY, "START photo mode   X shoot   TRIANGLE hide UI");
        for (int i = 0; i < MAX_BLIPS; i++) if (blips[i].active) { tx = blips[i].pos.x; tz = blips[i].pos.z; have = 1; break; }
    } else {
        float bd = 1e30f;
        for (int i = 0; i < MAX_MISSIONS; i++) if (!mission_done[i]) {
            float d = dist2d(player.pos.x, player.pos.z, MISSIONS[i].trigger.x, MISSIONS[i].trigger.z);
            if (d < bd) { bd = d; tx = MISSIONS[i].trigger.x; tz = MISSIONS[i].trigger.z; have = 1; }
            if (d < 30.0f && player.veh < 0) text(110, 196, C_WHITE, "Mission: %s (step into the marker)", MISSIONS[i].name);
        }
    }
    if (have) {                                                  /* compass arrow to nearest objective */
        float rel = wrap_pi(atan2f(tx - player.pos.x, tz - player.pos.z) - (cam_yaw + look_yaw));
        char a = fabsf(rel) < 0.6f ? '^' : (rel < 0 && rel > -2.2f) ? '>' : (rel > 0 && rel < 2.2f) ? '<' : 'v';
        text(8, 56, C_YELLOW, "%c %dm", a, (int)dist2d(player.pos.x, player.pos.z, tx, tz));
    }
    if (banner_timer > 0.0f) { int w = (int)strlen(banner_text) * 8; text(240 - w/2, 100, C_YELLOW, "%s", banner_text); }
}

static void draw_hud(void) {
    if (status_timer > 0) text(20, 236, C_YELLOW, "%s", status_msg);
    switch (current_state) {
    case STATE_MAIN_MENU: {
        static const char *items[4] = { "STORY MODE", "FREE ROAM", "GALLERY", "CONTROLS" };
        text(150, 40, C_YELLOW, "P R O J E C T   L I W A");
        text(160, 54, C_WHITE, "Liwa Desert Sandbox (PSP)");
        for (int i = 0; i < 4; i++) text(190, 110 + i*18, i == menu_sel ? C_YELLOW : C_WHITE, "%s %s", i == menu_sel ? ">" : " ", items[i]);
        text(150, 250, C_GREY, "UP/DOWN select    X confirm");
        break; }
    case STATE_GAMEPLAY: {
        text(8, 6, C_GREEN, "$%u", (unsigned)player_money);
        char st[6] = "-----"; for (int i = 0; i < wanted.wanted_stars; i++) st[i] = '*';
        text(8, 18, wanted.wanted_stars ? C_RED : C_GREY, "WANTED %s", st);
        text(8, 30, C_WHITE, "HP %d", player.health > 0 ? player.health : 0);
        put_rect(60, 31, clampf((float)player.health, 0, 100) * 0.8f, 6, RGB565(220,50,50));
        if (player.veh >= 0) {
            const Vehicle *v = &vehicles[player.veh];
            text(300, 6, C_WHITE, "%s", VSPEC[v->type].name);
            text(300, 18, C_WHITE, "%3d km/h  HP %d", (int)(fabsf(v->speed) * 3.6f), (int)v->health);
            if (VSPEC[v->type].cls == CLS_AIR) {
                text(300, 30, C_WHITE, "ALT %3dm  THR %3d%%", (int)(v->pos.y - terrain_height(v->pos.x, v->pos.z)), (int)(v->throttle * 100));
                if (v->speed < VSPEC[v->type].stall_speed && v->pos.y > terrain_height(v->pos.x, v->pos.z) + 1.0f) text(200, 60, C_RED, "STALL");
            }
        } else if (player.weapon > 0) text(300, 6, C_WHITE, "%s  %d", WNAME[player.weapon], player.ammo[player.weapon]);
        if (player.veh >= 0 && player.weapon > 0) text(300, 42, C_GREY, "%s %d", WNAME[player.weapon], player.ammo[player.weapon]);
        if (race.active) text(8, 44, C_YELLOW, "RACE %d/%d  %02d.%d s", race.next, MAX_RACE_CKPT, (int)race.time_left, (int)(race.time_left * 10) % 10);
        if (player.veh < 0) {                                      /* context prompt */
            int shop = shop_in_reach();
            if (shop >= 0) text(110, 210, C_WHITE, "%s: %s", btn_name(ctl.interact), POIS[shop].name);
        }
        if (fp_mode) {                                              /* scope overlay */
            put_rect(0, 0, 80, SCR_H, 0x0000); put_rect(400, 0, 80, SCR_H, 0x0000);
            put_rect(80, 135, 320, 1, 0x0000); put_rect(239, 0, 1, SCR_H, 0x0000); put_rect(236, 133, 7, 5, C_RED);
        } else text(8, 258, C_GREY, "L look (tap=recenter)  R lock-on  SELECT cam  START photo");
        draw_mission_hud();
        if (resp_state != RESP_NONE) text(190, 120, C_RED, resp_state == RESP_WASTED ? "W A S T E D" : "B U S T E D");
        break; }
    case STATE_PHOTO_MODE:
        if (!photo_hide_ui) {
            text(8, 6, C_WHITE, "PHOTO MODE  (world paused)");
            text(8, 18, C_GREY, "Stick move  L/R down/up  D-pad look  []=fast");
            text(8, 30, C_GREY, "X capture  /\\ hide UI  START exit");
        }
        break;
    case STATE_INTERIOR: {
        text(20, 20, C_YELLOW, "%s", POIS[store_poi].name);
        text(20, 34, C_GREEN, "Cash: $%u", (unsigned)player_money);
        if (store_type == STORE_DEALERSHIP) {
            for (int i = 0; i < 2; i++) text(30, 70 + i*16, i == store_sel ? C_YELLOW : C_WHITE, "%s %-20s $%d", i == store_sel ? ">" : " ", VSPEC[SHOP_DEALER_TYPES[i]].name, VSPEC[SHOP_DEALER_TYPES[i]].price);
        } else if (store_type == STORE_GUNSHOP) {
            static const char *nm[6] = { "Pistol (+24)", "Assault Rifle (+60)", "Sniper Rifle (+10)", "Pistol ammo x24", "Rifle ammo x60", "Sniper ammo x10" };
            static const int pr[6] = { 500, 4000, 7500, 150, 400, 300 };
            for (int i = 0; i < 6; i++) text(30, 70 + i*16, i == store_sel ? C_YELLOW : C_WHITE, "%s %-22s $%d", i == store_sel ? ">" : " ", nm[i], pr[i]);
            text(30, 180, C_GREY, "Ammo: pistol %d  rifle %d  sniper %d", player.ammo[1], player.ammo[2], player.ammo[3]);
        } else {
            if (bank_cooldown > 0) text(30, 90, C_RED, "Vault is empty. Try again in %ds", (int)bank_cooldown);
            else { text(30, 80, C_WHITE, "HOLD X to crack the vault (triggers the alarm)"); put_rect(30, 100, 200, 10, RGB565(60,60,60)); put_rect(30, 100, (int)(clampf(heist_prog, 0, 1) * 200), 10, RGB565(240,200,40)); }
        }
        text(20, 236 - 16, C_GREY, "UP/DOWN select  X confirm  O leave");
        break; }
    case STATE_GALLERY:
        text(20, 16, C_YELLOW, "GALLERY  (%s)  %d photo(s)", PHOTO_DIR, gallery_count);
        if (!gallery_count) text(20, 60, C_GREY, "No photos yet. Press START in-game, then X.");
        for (int i = 0; i < 20 && gallery_sel - (gallery_sel % 20) + i < gallery_count; i++) {
            int k = gallery_sel - (gallery_sel % 20) + i;
            text(20, 40 + i*10, k == gallery_sel ? C_YELLOW : C_WHITE, "%s %-24s %d KB", k == gallery_sel ? ">" : " ", gallery[k].name, gallery[k].size / 1024);
        }
        text(20, 256, C_GREY, "UP/DOWN scroll   O back");
        break;
    case STATE_CONTROLS_CONFIG: {
        static const char *nm[4] = { "Accelerate / Sprint", "Brake / Fire", "Handbrake / Jump", "Enter-Exit / Interact" };
        text(20, 20, C_YELLOW, "CONTROLS");
        for (int i = 0; i < 4; i++) text(30, 60 + i*18, i == ctl_row ? C_YELLOW : C_WHITE, "%s %-22s %s", i == ctl_row ? ">" : " ", nm[i], (ctl_listen && i == ctl_row) ? "press a face button..." : btn_name(*ctl_slot(i)));
        text(20, 220, C_GREY, "X rebind  SELECT defaults  O back");
        break; }
    default: break;
    }
}

/* Dump the just-rendered 5650 draw buffer to a 24-bit BMP (before HUD is drawn). */
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static int save_screenshot(void) {
    static uint8_t row[SCR_W * 3];
    char path[96]; int n = photo_counter;
    sceIoMkdir("ms0:/PSP/PHOTO", 0777); sceIoMkdir(PHOTO_DIR, 0777);
    for (; n < 10000; n++) {
        snprintf(path, sizeof(path), PHOTO_DIR "/LIWA_%04d.bmp", n);
        SceUID t = sceIoOpen(path, PSP_O_RDONLY, 0); if (t < 0) break; sceIoClose(t);
    }
    photo_counter = n + 1;
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777); if (fd < 0) return 0;
    uint8_t h[54]; memset(h, 0, sizeof(h));
    h[0] = 'B'; h[1] = 'M'; put32(h + 2, 54 + SCR_W*SCR_H*3); put32(h + 10, 54); put32(h + 14, 40);
    put32(h + 18, SCR_W); put32(h + 22, SCR_H); h[26] = 1; h[28] = 24; put32(h + 34, SCR_W*SCR_H*3);
    sceIoWrite(fd, h, 54);
    const uint16_t *src = fb();
    for (int y = SCR_H - 1; y >= 0; y--) {                       /* BMP is bottom-up */
        for (int x = 0; x < SCR_W; x++) {
            uint16_t p = src[y*BUF_W + x]; int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            row[x*3] = (b << 3) | (b >> 2); row[x*3+1] = (g << 2) | (g >> 4); row[x*3+2] = (r << 3) | (r >> 2);
        }
        sceIoWrite(fd, row, sizeof(row));
    }
    sceIoClose(fd); return 1;
}

/* ------------------------------------------------------------------------ */
/*  Kernel callbacks + main                                                  */
/* ------------------------------------------------------------------------ */
static int exit_callback(int a1, int a2, void *common) { running = 0; sceKernelExitGame(); return 0; }
static int callback_thread(SceSize args, void *argp) {
    int cb = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb); sceKernelSleepThreadCB(); return 0;
}
static void setup_callbacks(void) {
    int t = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (t >= 0) sceKernelStartThread(t, 0, 0);
}

int main(void) {
    setup_callbacks(); init_graphics();
    sceCtrlSetSamplingCycle(0); sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    controls_defaults(); reset_world(0);

    SceCtrlData pad; uint32_t old = 0;
    uint64_t last = sceKernelGetSystemTimeWide();

    while (running) {
        sceCtrlPeekBufferPositive(&pad, 1);
        uint32_t pressed = pad.Buttons & ~old; g_released = old & ~pad.Buttons; old = pad.Buttons;
        uint64_t now = sceKernelGetSystemTimeWide();
        float dt = clampf((float)(now - last) / 1000000.0f, 0.004f, 0.05f); last = now;
        if (current_state != STATE_GAMEPLAY) game_time += dt;

        switch (current_state) {
        case STATE_MAIN_MENU:
            update_chunks(CITY_CX, CITY_CZ);
            if (pressed & PSP_CTRL_UP)   menu_sel = (menu_sel + 3) % 4;
            if (pressed & PSP_CTRL_DOWN) menu_sel = (menu_sel + 1) % 4;
            if (pressed & PSP_CTRL_CROSS) {
                if (menu_sel == 0) enter_gameplay(1);
                if (menu_sel == 1) enter_gameplay(0);
                if (menu_sel == 2) { gallery_scan(); current_state = STATE_GALLERY; }
                if (menu_sel == 3) { ctl_row = 0; ctl_listen = 0; current_state = STATE_CONTROLS_CONFIG; }
            }
            break;
        case STATE_GAMEPLAY:      update_gameplay(dt, &pad, pressed); break;
        case STATE_PHOTO_MODE:    update_photo(dt, &pad, pressed); break;
        case STATE_INTERIOR:      update_interior(dt, pad.Buttons, pressed); break;
        case STATE_GALLERY:       update_gallery(pressed); break;
        case STATE_CONTROLS_CONFIG: update_controls(pressed); break;
        }

        render_frame();
        if (current_state == STATE_PHOTO_MODE && capture_req) {           /* clean frame, no HUD */
            capture_req = 0; set_status(save_screenshot() ? "Photo saved to " PHOTO_DIR : "Could not write photo");
        }
        if (!(current_state == STATE_PHOTO_MODE && photo_hide_ui && status_timer <= 0.0f)) draw_hud();
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
        draw_off = (draw_off == 0) ? FRAME_SIZE : 0;
    }
    sceKernelExitGame();
    return 0;
}
