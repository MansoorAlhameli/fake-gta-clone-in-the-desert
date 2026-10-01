/**
 * ============================================================================
 * ENGINE ARCHITECTURE BLUEPRINT: PROJECT LIWA SANDBOX (PSP TARGET HARDWARE)
 * Target Architecture: Allegrex MIPS @ 333MHz | VRAM: 2MB | Main RAM: 32MB
 * ============================================================================
 */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <string.h>
#include <math.h>

PSP_MODULE_INFO("LiwaSandbox", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

/* --- HARDWARE CONSTANTS & MEMORY ALLOCATION CONSTRAINTS --- */
#define BUF_WIDTH     512
#define SCR_WIDTH     480
#define SCR_HEIGHT    272
#define VRAM_SIZE     (0x44000000)

#define MAX_VEHICLES  16
#define MAX_NPCS      32
#define MAX_RACECKPT  8
#define MAX_CHUNKS    16

/* --- STATE MACHINE ENUMS --- */
typedef enum {
    STATE_MAIN_MENU,
    STATE_GALLERY,
    STATE_CONTROLS_CONFIG,
    STATE_GAMEPLAY,
    STATE_PHOTO_MODE,
    STATE_INTERIOR
} GameState;

typedef enum {
    VEHICLE_NONE,
    VEHICLE_LC100,
    VEHICLE_LC200,
    VEHICLE_Y60,
    VEHICLE_Y61,
    VEHICLE_Y62,
    VEHICLE_PLANE,
    VEHICLE_DIRT_BIKE,
    VEHICLE_QUAD
} VehicleType;

typedef enum {
    STORE_NONE,
    STORE_DEALERSHIP,
    STORE_GUNSHOP,
    STORE_BANK_VAULT,
    STORE_HOSPITAL
} StoreType;

typedef enum {
    ZONE_DUNES,
    ZONE_AIRPORT,
    ZONE_CITY
} ZoneType;

/* --- COMPONENT VECTOR STRUCTS --- */
typedef struct {
    float x, y, z;
} Vector3D;

/* --- CONTROL CONFIGURATION STRUCTURE (GTA: LCS PC ADAPTATION) --- */
typedef struct {
    unsigned int btn_accelerate;
    unsigned int btn_brake;
    unsigned int btn_handbrake;
    unsigned int btn_interact;
    unsigned int btn_attack;
    unsigned int btn_lockon;
} ControlConfig;

/* --- OBJECT POOL STRUCTURES (ZERO-MALLOC STATIC ALLOCATION) --- */
typedef struct {
    VehicleType type;
    Vector3D position;
    Vector3D rotation;
    float current_speed;
    float max_speed;
    float acceleration;
    float weight;
    float traction;
    float health;
    int is_active;
} Vehicle;

typedef struct {
    Vector3D position;
    int health;
    int behavior_state; /* 0 = Wander, 1 = Flee, 2 = Aggressive */
    int is_active;
} NPC;

typedef struct {
    Vector3D position;
    int is_triggered;
} RaceCheckpoint;

typedef struct {
    Vector3D position;
    float radius;
    StoreType type;
} InteriorTrigger;

typedef struct {
    ZoneType type;
    Vector3D origin;
    int is_loaded; /* 1 if currently in memory/rendering bounds */
} MapChunk;

typedef struct {
    Vector3D position;
    float rotation_y;
    int health;
    int current_weapon; /* 0 = Unarmed, 1 = Pistol, 2 = Assault Rifle */
    int in_vehicle_index; /* -1 if on foot */
} PlayerState;

typedef struct {
    uint8_t wanted_stars; /* 0 to 5 STARS */
    float heat_level;     /* Meter filling towards next star */
    int police_spawn_timer;
} WantedSystem;

/* --- ENGINE GLOBAL VARIABLES (STATICALLY ALLOCATED MEMORY ARENAS) --- */
static unsigned int __attribute__((aligned(16))) list[262144]; /* GU display list allocation */
static GameState current_state       = STATE_MAIN_MENU;
static PlayerState player            = { {240.0f, 0.0f, -100.0f}, 0.0f, 100, 0, -1 };
static WantedSystem police_engine    = { 0, 0.0f, 0 };
static ControlConfig inputs          = { PSP_CTRL_CROSS, PSP_CTRL_SQUARE, PSP_CTRL_RTRIGGER, PSP_CTRL_TRIANGLE, PSP_CTRL_CIRCLE, PSP_CTRL_LTRIGGER };
static uint32_t player_money         = 15000; /* Initial currency wallet balance */
static int selected_menu_item        = 0;
static int is_story_mode             = 0;
static StoreType active_store_type   = STORE_NONE;

/* Static Memory Pools */
static Vehicle vehicle_pool[MAX_VEHICLES];
static NPC npc_pool[MAX_NPCS];
static RaceCheckpoint race_checkpoints[MAX_RACECKPT];
static InteriorTrigger triggers[8];
static MapChunk world_chunks[MAX_CHUNKS];

/* Photo Mode Free Fly Vector Offsets */
static Vector3D photo_cam_pos = {0.0f, 20.0f, 0.0f};
static Vector3D photo_cam_rot = {0.0f, 0.0f, 0.0f};

/* Hardware Vertex Format (Position + Color) */
struct Vertex {
    unsigned int color;
    float x, y, z;
};

/* --- EXIT KERNEL CALLBACK ROUTINES --- */
int exit_callback(int arg1, int arg2, void *common) {
    sceKernelExitGame();
    return 0;
}

int callback_thread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

void setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if(thid >= 0) sceKernelStartThread(thid, 0, 0);
}

/* --- HARDWARE GRAPHICS UTILITY INITIALIZATION --- */
void init_graphics(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    
    /* 16-bit color depth (5650) to maximize VRAM for textures */
    sceGuDrawBuffer(GU_PSM_5650, (void*)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void*)0x88000, BUF_WIDTH);
    sceGuDepthBuffer((void*)0x110000, BUF_WIDTH);
    
    sceGuOffset(2048 - (SCR_WIDTH/2), 2048 - (SCR_HEIGHT/2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_CULL_FACE);
    
    sceGuFinish();
    sceGuSync(0,0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

/* --- VEHICLE DATA LOADER INTERFACE --- */
void initialize_vehicle_preset(int index, VehicleType type, float x, float z) {
    vehicle_pool[index].type = type;
    vehicle_pool[index].position.x = x;
    vehicle_pool[index].position.y = 0.0f; /* Clamped to dynamic height map later */
    vehicle_pool[index].position.z = z;
    vehicle_pool[index].rotation.y = 0.0f;
    vehicle_pool[index].current_speed = 0.0f;
    vehicle_pool[index].health = 1000.0f;
    vehicle_pool[index].is_active = 1;

    switch(type) {
        case VEHICLE_LC100:
        case VEHICLE_LC200:
            vehicle_pool[index].max_speed = 4.5f;   vehicle_pool[index].acceleration = 0.08f;
            vehicle_pool[index].weight    = 2500.0f; vehicle_pool[index].traction     = 0.85f;
            break;
        case VEHICLE_Y60:
        case VEHICLE_Y61:
        case VEHICLE_Y62:
            vehicle_pool[index].max_speed = 4.8f;   vehicle_pool[index].acceleration = 0.09f;
            vehicle_pool[index].weight    = 2400.0f; vehicle_pool[index].traction     = 0.90f;
            break;
        case VEHICLE_PLANE:
            vehicle_pool[index].max_speed = 12.0f;  vehicle_pool[index].acceleration = 0.15f;
            vehicle_pool[index].weight    = 1800.0f; vehicle_pool[index].traction     = 0.30f; /* Pitch/Lift mechanics */
            break;
        case VEHICLE_DIRT_BIKE:
        case VEHICLE_QUAD:
            vehicle_pool[index].max_speed = 5.2f;   vehicle_pool[index].acceleration = 0.18f;
            vehicle_pool[index].weight    = 350.0f;  vehicle_pool[index].traction     = 0.95f;
            break;
        default:
            vehicle_pool[index].is_active = 0;
            break;
    }
}

/* --- SYSTEM SANDBOX POOLS SETUP INITIALIZATION --- */
void init_game_world(void) {
    /* Setup Map Grid (Liwa Dunes, Airport, City) */
    world_chunks[0] = (MapChunk){ZONE_DUNES, {0.0f, 0.0f, 0.0f}, 1};
    world_chunks[1] = (MapChunk){ZONE_AIRPORT, {1000.0f, 0.0f, -1000.0f}, 0};
    world_chunks[2] = (MapChunk){ZONE_CITY, {500.0f, 0.0f, 500.0f}, 0};

    /* Vehicles */
    initialize_vehicle_preset(0, VEHICLE_LC200, 100.0f, -200.0f);
    initialize_vehicle_preset(1, VEHICLE_Y61, -50.0f, -400.0f);
    initialize_vehicle_preset(2, VEHICLE_PLANE, 1000.0f, -1000.0f); 
    initialize_vehicle_preset(3, VEHICLE_QUAD, 10.0f, 50.0f);

    /* Interiors & Stores */
    triggers[0] = (InteriorTrigger){ {500.0f, 0.0f, 520.0f}, 10.0f, STORE_BANK_VAULT };
    triggers[1] = (InteriorTrigger){ {550.0f, 0.0f, 580.0f}, 10.0f, STORE_DEALERSHIP };
    triggers[2] = (InteriorTrigger){ {480.0f, 0.0f, 450.0f}, 15.0f, STORE_HOSPITAL };

    /* Zero out NPCs */
    for(int i = 0; i < MAX_NPCS; i++) {
        npc_pool[i].is_active = (i < 5) ? 1 : 0;
        npc_pool[i].position.x = (float)(i * 30) - 150.0f;
        npc_pool[i].position.y = 0.0f;
        npc_pool[i].position.z = -300.0f;
        npc_pool[i].health = 100;
        npc_pool[i].behavior_state = 0;
    }
}

/* --- GAME LOGIC & SYSTEMS SUBSYSTEMS --- */
void trigger_busted_wasted() {
    player.health = 100;
    player.in_vehicle_index = -1;
    player_money = (player_money > 1000) ? player_money - 1000 : 0; /* Penalty */
    police_engine.wanted_stars = 0;
    police_engine.heat_level = 0.0f;
    /* Teleport to nearest hospital */
    player.position = triggers[2].position; 
}

void check_interiors() {
    if(player.in_vehicle_index != -1) return; /* Must be on foot */
    for(int i=0; i<8; i++) {
        if(triggers[i].type != STORE_NONE) {
            float dx = player.position.x - triggers[i].position.x;
            float dz = player.position.z - triggers[i].position.z;
            if(sqrtf(dx*dx + dz*dz) < triggers[i].radius) {
                active_store_type = triggers[i].type;
                current_state = STATE_INTERIOR;
                return;
            }
        }
    }
}

/* --- LOW LEVEL HARDWARE INPUT HANDLING MODULE --- */
void process_gameplay_input(SceCtrlData *pad) {
    int lx = pad->Lx - 128;
    int ly = pad->Ly - 128;
    
    /* Health check */
    if (player.health <= 0) {
        trigger_busted_wasted();
        return;
    }

    if(player.in_vehicle_index == -1) {
        /* On-Foot Physics Calculation System Loop */
        if(abs(lx) > 20) player.rotation_y += (float)lx * 0.0005f;
        if(abs(ly) > 20) {
            player.position.x += sinf(player.rotation_y) * ((float)-ly * 0.03f);
            player.position.z += cosf(player.rotation_y) * ((float)-ly * 0.03f);
        }
        
        /* Entering proximity vehicle detection mechanism via Triangle Button */
        if(pad->Buttons & inputs.btn_interact) {
            for(int i = 0; i < MAX_VEHICLES; i++) {
                if(vehicle_pool[i].is_active) {
                    float dx = player.position.x - vehicle_pool[i].position.x;
                    float dz = player.position.z - vehicle_pool[i].position.z;
                    if((dx*dx + dz*dz) < 100.0f) { 
                        player.in_vehicle_index = i;
                        break;
                    }
                }
            }
            sceKernelDelayThread(200000); /* Debounce */
        }
        check_interiors();
    } else {
        /* In-Vehicle Matrix/Physics Manipulation Simulation Loop */
        Vehicle *v = &vehicle_pool[player.in_vehicle_index];
        
        if(abs(lx) > 20) v->rotation.y -= (float)lx * (0.0003f * v->traction * (v->current_speed/v->max_speed));
        
        if(pad->Buttons & inputs.btn_accelerate) {
            if(v->current_speed < v->max_speed) v->current_speed += v->acceleration;
        } else if(pad->Buttons & inputs.btn_brake) {
            if(v->current_speed > -v->max_speed/2) v->current_speed -= v->acceleration * 1.5f;
        } else {
            v->current_speed *= 0.98f; /* Natural deceleration drag */
        }

        /* Specialized Aviation Handling */
        if(v->type == VEHICLE_PLANE && v->current_speed > 6.0f) {
            if(abs(ly) > 20) v->position.y += ((float)ly * 0.02f); /* Pitch control via Analog Y */
        } else {
            /* Gravity / Dune mapping clamp */
            if(v->position.y > 0.0f) v->position.y -= 0.5f;
            if(v->position.y < 0.0f) v->position.y = 0.0f;
        }
        
        /* Apply dynamic translation vectors */
        v->position.x += sinf(v->rotation.y) * v->current_speed;
        v->position.z += cosf(v->rotation.y) * v->current_speed;
        
        player.position = v->position;
        
        if(pad->Buttons & inputs.btn_interact) {
            player.position.x -= 10.0f; /* Prevent clipping */
            player.in_vehicle_index = -1;
            sceKernelDelayThread(200000); /* Debounce */
        }
    }
}

void process_photo_mode(SceCtrlData *pad) {
    int lx = pad->Lx - 128;
    int ly = pad->Ly - 128;
    
    if(abs(lx) > 20) photo_cam_rot.y += (float)lx * 0.001f;
    if(abs(ly) > 20) {
        photo_cam_pos.x += sinf(photo_cam_rot.y) * ((float)-ly * 0.1f);
        photo_cam_pos.z += cosf(photo_cam_rot.y) * ((float)-ly * 0.1f);
    }
    if(pad->Buttons & PSP_CTRL_RTRIGGER) photo_cam_pos.y += 1.0f;
    if(pad->Buttons & PSP_CTRL_LTRIGGER) photo_cam_pos.y -= 1.0f;
    
    if(pad->Buttons & PSP_CTRL_SELECT) {
        current_state = STATE_GAMEPLAY;
        sceKernelDelayThread(200000);
    }
}

/* --- RENDERING HARDWARE STATE SWITCH LOOP SYSTEM --- */
void render_scene(void) {
    sceGuStart(GU_DIRECT, list);
    sceGuClearColor(0xFFEBC187); /* Desert Skybox Clear */
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(75.0f, 16.0f/9.0f, 0.5f, 5000.0f);
    
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();

    switch(current_state) {
        case STATE_MAIN_MENU:
            pspDebugScreenSetXY(15, 8);
            pspDebugScreenPrintf("--- PROJECT LIWA SANDBOX ---");
            pspDebugScreenSetXY(18, 11);
            pspDebugScreenPrintf((selected_menu_item == 0) ? "> STORY MODE <" : "  STORY MODE  ");
            pspDebugScreenSetXY(18, 12);
            pspDebugScreenPrintf((selected_menu_item == 1) ? "> FREE ROAM <" : "  FREE ROAM  ");
            pspDebugScreenSetXY(18, 13);
            pspDebugScreenPrintf((selected_menu_item == 2) ? "> GALLERY <" : "  GALLERY  ");
            pspDebugScreenSetXY(18, 14);
            pspDebugScreenPrintf((selected_menu_item == 3) ? "> CONTROLS CONFIG <" : "  CONTROLS CONFIG  ");
            break;

        case STATE_GAMEPLAY: 
        case STATE_PHOTO_MODE: {
            ScePspFVector3 cam_pos, look_at;
            ScePspFVector3 up_vec = { 0.0f, 1.0f, 0.0f };

            if(current_state == STATE_GAMEPLAY) {
                cam_pos.x = player.position.x - sinf(player.rotation_y)*40.0f;
                cam_pos.y = player.position.y + 15.0f;
                cam_pos.z = player.position.z - cosf(player.rotation_y)*40.0f;
                look_at.x = player.position.x;
                look_at.y = player.position.y + 5.0f;
                look_at.z = player.position.z;
            } else {
                cam_pos.x = photo_cam_pos.x;
                cam_pos.y = photo_cam_pos.y;
                cam_pos.z = photo_cam_pos.z;
                look_at.x = photo_cam_pos.x + sinf(photo_cam_rot.y);
                look_at.y = photo_cam_pos.y;
                look_at.z = photo_cam_pos.z + cosf(photo_cam_rot.y);
            }
            
            sceGumLookAt(&cam_pos, &look_at, &up_vec);

            sceGumMatrixMode(GU_MODEL);
            
            /* T&L via VFPU Matrix Stack: Draw Vehicles */
            for(int i=0; i<MAX_VEHICLES; i++) {
                if(vehicle_pool[i].is_active) {
                    sceGumPushMatrix();
                    ScePspFVector3 v_pos = {vehicle_pool[i].position.x, vehicle_pool[i].position.y, vehicle_pool[i].position.z};
                    sceGumTranslate(&v_pos);
                    sceGumRotateY(vehicle_pool[i].rotation.y);
                    
                    /* Hardware GU draw call would go here (e.g., sceGumDrawArray) */
                    
                    sceGumPopMatrix();
                }
            }

            /* UI HUD Overlays System Data Mapping Displays */
            pspDebugScreenSetXY(2, 2);
            if(current_state == STATE_GAMEPLAY) {
                pspDebugScreenPrintf("Wallet: $%07d | Heat: %d Stars | HP: %d", player_money, police_engine.wanted_stars, player.health);
                pspDebugScreenSetXY(2, 4);
                if(player.in_vehicle_index == -1) {
                    pspDebugScreenPrintf("State: On Foot | X: %.1f Z: %.1f", player.position.x, player.position.z);
                } else {
                    pspDebugScreenPrintf("Driving: %d | Speed: %.1f MPH", player.in_vehicle_index, vehicle_pool[player.in_vehicle_index].current_speed * 10.0f);
                }
            } else {
                pspDebugScreenPrintf("[PHOTO MODE] L/R Triggers: Height | Analog: Move | SELECT: Exit");
            }
            break;
        }

        case STATE_INTERIOR:
            pspDebugScreenSetXY(15, 8);
            if(active_store_type == STORE_BANK_VAULT) pspDebugScreenPrintf("--- BANK VAULT: Press [] to Start Heist ---");
            if(active_store_type == STORE_DEALERSHIP) pspDebugScreenPrintf("--- AL FUTTAIM MOTORS: Purchase Y61 ($40k) ---");
            if(active_store_type == STORE_HOSPITAL) pspDebugScreenPrintf("--- LIWA GENERAL HOSPITAL ---");
            pspDebugScreenSetXY(15, 10);
            pspDebugScreenPrintf("Press Triangle to Exit Interior");
            break;

        default:
            break;
    }
    
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

/* --- MONOLITHIC EXECUTIVE PIPELINE INITIALIZER --- */
int main(void) {
    SceCtrlData pad;
    setup_callbacks();
    init_graphics();
    init_game_world();
    pspDebugScreenInit();
    
    while(1) {
        sceCtrlReadBufferPositive(&pad, 1);
        
        switch(current_state) {
            case STATE_MAIN_MENU:
                if(pad.Buttons & PSP_CTRL_UP) { selected_menu_item = (selected_menu_item - 1 + 4) % 4; sceKernelDelayThread(150000); }
                if(pad.Buttons & PSP_CTRL_DOWN) { selected_menu_item = (selected_menu_item + 1) % 4; sceKernelDelayThread(150000); }
                if(pad.Buttons & PSP_CTRL_CROSS) {
                    if(selected_menu_item == 0) { is_story_mode = 1; current_state = STATE_GAMEPLAY; }
                    if(selected_menu_item == 1) { is_story_mode = 0; current_state = STATE_GAMEPLAY; }
                    if(selected_menu_item == 2) { current_state = STATE_GALLERY; }
                    if(selected_menu_item == 3) { current_state = STATE_CONTROLS_CONFIG; }
                    sceKernelDelayThread(200000);
                }
                break;
                
            case STATE_GAMEPLAY:
                process_gameplay_input(&pad);
                if(pad.Buttons & PSP_CTRL_SELECT) {
                    current_state = STATE_PHOTO_MODE;
                    photo_cam_pos = player.position;
                    photo_cam_rot.y = player.rotation_y;
                    sceKernelDelayThread(200000);
                }
                break;
                
            case STATE_PHOTO_MODE:
                process_photo_mode(&pad);
                break;

            case STATE_INTERIOR:
                if(pad.Buttons & PSP_CTRL_TRIANGLE) {
                    player.position.z -= 20.0f; /* Push outside trigger */
                    current_state = STATE_GAMEPLAY;
                    sceKernelDelayThread(200000);
                }
                if(pad.Buttons & PSP_CTRL_SQUARE && active_store_type == STORE_BANK_VAULT) {
                    player_money += 50000;
                    police_engine.wanted_stars = 4; /* Heist triggers 4 stars */
                    player.position.z -= 20.0f;
                    current_state = STATE_GAMEPLAY;
                    sceKernelDelayThread(200000);
                }
                break;
                
            default:
                break;
        }
        
        render_scene();
    }
    
    return 0;
}