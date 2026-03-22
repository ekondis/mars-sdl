#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAP_IDX_CAP(idx) (((size_t)(idx) % 0x10000))
#define MAP_IDX(x, y) MAP_IDX_CAP(((y) * 256 + (x)))

#define CLAMP(v, min, max) ((v) < (min) ? (min) : ((v) > (max) ? (max) : (v)))

// Global variables matching original ASM data section
uint8_t *landscape_data;             // _0345 dw 024d1h
uint8_t *aux_array;                  // _0347 dw 034d1h
uint8_t *terra_data;                 // _0349 dw 044d1h
uint8_t *sky_data;                   // _034b dw 054d1h
const uint16_t const_cf85 = 0x0cf85; // _034d dw 0cf85h

// Variables corresponding to specific memory locations in the original code.
// These control the camera/player position and other state.
uint16_t var_0355;     // _0355 Player X position
uint16_t var_0357;     // _0357 Player Y position (or angle)
uint16_t var_0359;     // _0359 Player Z position / horizon height
uint16_t var_035d;     // _035d Random seed
uint16_t var_035d_tmp; // _035d Random seed (tmp)

// Table loaded by `_load1` to `_load4` and consumed by terrain rendering.
#define TERRAIN_RAY_TABLE_COUNT                                                \
  (200 + 0x29 + 0x0a + 0x0a)
uint16_t g_terrain_ray_table[TERRAIN_RAY_TABLE_COUNT];

// Per-column terrain render scratch buffers.
int16_t g_terrain_column_heights[256];
uint16_t g_terrain_column_colors[256];

uint8_t key_pressed = 0;
uint8_t mouse_available = 0;

// SDL globals
SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *texture;
uint32_t palette[256];

// Random number generator state (matching original)
uint16_t random_seed;

// Function prototypes
void init_memory_segments();
void cleanup_memory_segments();
void load_palette();
void prepare_sky();      // _00f3
void prepare_terrain();  // _0153
void handle_input();     // _0408
void calculate_height(); // _105c
void render_sky();       // _0459
void render_terrain();   // _0a1f
void diamond_square(uint8_t *buffer, uint16_t full_size, int start_x,
                    int start_y); // _0219
int cr05_subroutine(uint8_t center_val, int step_size);
uint16_t cr02_function(unsigned int terrain_idx, uint8_t frac_step);
uint8_t cr03_subroutine(uint8_t *buffer, unsigned int src_idx,
                        unsigned int dst_idx,
                        uint8_t half_step, unsigned int ref_val);

// Initialize memory segments to match original ASM layout
void init_memory_segments() {
  // Allocate memory segments
  landscape_data =
      (uint8_t *)calloc(256 * 200,
                        1); // 50KB for landscape (256x200 pixels)
  aux_array = (uint8_t *)calloc(0x10000, 1); // terrain height map (originally 64KB aux data)
  terra_data = (uint8_t *)calloc(0x10000, 1); // terrain slope map (originally 64KB terra data)
  sky_data = (uint8_t *)calloc(0x10000, 1);   // originally 64KB for sky
}

/**
 * Initializes global data structures.
 * Corresponds to the `_load1` to `_load4` loops in the `start` section.
 */
void init_data() {
  uint16_t value = 0;
  int out_idx = 0;

  // `_load1` loop
  value = 0x0b3e;
  for (int i = 0; i < 200; i++) {
    g_terrain_ray_table[out_idx++] = value;
    value += 0x0006;
  }

  // `_load2` loop
  value = 0;
  for (int i = 0; i < 0x29; i++) {
    g_terrain_ray_table[out_idx++] = value;
    value++;
  }

  // `_load3` loop
  value = 0x2b;
  for (int i = 0; i < 0x0a; i++) {
    g_terrain_ray_table[out_idx++] = value;
    value += 2;
  }

  // `_load4` loop
  value = 0x40;
  for (int i = 0; i < 0x0a; i++) {
    g_terrain_ray_table[out_idx++] = value;
    value += 4;
  }
}

void cleanup_memory_segments() {
  free(landscape_data);
  free(aux_array);
  free(terra_data);
  free(sky_data);
}

// Load palette - original code starting at p1:
void load_palette() {
  uint16_t vga_color = 0;
  uint8_t palette_idx = 0;

  // First palette section (terra shades)
  for (int pass = 0; pass < 2; pass++) {
    for (uint8_t cl = 0; cl < 0x40; cl++) {
      // VGA DAC values are 6-bit (0–63), so scale to 8-bit (0–255) by shifting
      // (<< 2)
      uint8_t r = (vga_color & 0xFF) << 2;
      uint8_t g = ((vga_color >> 8) & 0xFF) << 2;
      uint8_t b = g; // Blue = Green in original

      palette[palette_idx] = (r << 24) | (g << 16) | (b << 8) | 0xFF;

      vga_color++;
      palette_idx++;
    }

    // add bx, 941eh ; new shades of sky
    vga_color += 0x941e;
  }

  // Fill remaining palette entries
  for (int i = 128; i < 256; i++) {
    palette[i] = 0x000000FF; // Black with alpha
  }
}

// Prepare cloudy sky - original _00f3
void prepare_sky() {
  // Original: _00f3:  mov es, cs:[_034b]
  uint8_t *sky_buf = sky_data;
  const uint16_t sky_size = (uint16_t)256;
  const uint16_t sky_half = sky_size / 2;

  // Original: xor di, di; mov ax, 0ffffh; mov cx, 8000h; rep stosw
  // Clear the sky buffer with 0xFF
  memset(sky_buf, 0xFF, 0x10000);

  // Original random number generation
  // mov ax, 00abh; mul word ptr cs:[_035d]; add ax, 2bcdh; adc dx, +00
  // div word ptr cs:[_034d]; mov cs:[_035d], dx
  uint32_t temp = (uint32_t)0x00ab * var_035d;
  temp += 0x2bcd;
  var_035d = temp % const_cf85;
  uint16_t rand_val = var_035d;

  // Original: Set corner values for diamond-square
  // mov byte ptr es:[0000h], 0
  // mov byte ptr es:[0080h], 0feh
  // mov byte ptr es:[8000h], 0feh
  // mov byte ptr es:[8080h], 0
  sky_buf[MAP_IDX(0, 0)] = 0;
  sky_buf[MAP_IDX(sky_half, 0)] = 0xfe;
  sky_buf[MAP_IDX(0, sky_half)] = 0xfe;
  sky_buf[MAP_IDX(sky_half, sky_half)] = 0;

  // Original: push +00; push 0100h; call _0219; add sp, +04
  var_035d_tmp = var_035d;
  diamond_square(sky_buf, sky_size, 0, 0);

  // Original: xor di, di
  // _0141:  mov al, es:[di]; shr al, 03; add al, 40h; stosb
  // or di, di; jnz short _0141
  for (size_t px_idx = 0; px_idx < 0x10000; px_idx++) {
    uint8_t pixel_val = sky_buf[px_idx];
    pixel_val >>= 3;
    pixel_val += 0x40;
    sky_buf[px_idx] = pixel_val;
  }
}

// Terrain generation (_0153 function)
void prepare_terrain(void) {
  // Original: mov es, cs:[_0347]; mov fs, cs:[_0349]
  uint8_t *height_buf = aux_array; // terrain height map
  uint8_t *slope_buf = terra_data; // terrain color/slope data

  // Original: xor di, di; mov ax, 0ffffh; mov cx, 8000h; rep stosw
  memset(height_buf, 0xFF, 0x10000);
  memset(slope_buf, 0xFF, 0x10000);

  // Original random generation - same as sky
  uint32_t temp = (uint32_t)0x00ab * var_035d;
  temp += 0x2bcd;
  var_035d = temp % const_cf85;

  // Original: mov byte ptr es:[0000], 0080h
  height_buf[0] = 0x80;

  // Original: push +00; push 0100h; call _0219; add sp, +04
  var_035d_tmp = var_035d;
  diamond_square(height_buf, (uint16_t)256, 0, 0);

  // Original smoothing pass: _019a loop
  // xor si, si
  // _019a: mov al, es:[si]; xor ah, ah; add al, es:[si+4]; adc ah, 00
  // add al, es:[si+0202h]; adc ah, 00; add al, es:[si+0feffh]; adc ah, 00
  // shr ax, 02; mov es:[si], al; inc si; jnz _019a
  for(int y = 0; y < 256; y++) {
    for(int x = 0; x < 256; x++) {
      size_t px_idx = MAP_IDX(x, y);
      int sum = height_buf[px_idx];
      sum += height_buf[MAP_IDX(x+4, y)];
      sum += height_buf[MAP_IDX(x+2, y+2)];
      sum += height_buf[MAP_IDX(x-1, y-1)];
      height_buf[px_idx] = sum / 4;
    }
  }

  // Original slope calculation: _01cc loop
  // xor si, si
  // _01cc: mov al, es:[si]; xor ah, ah; sub al, es:[si + 03]; sbb ah, 00
  // add ax, 0020h; jns short _01df; xor ax, ax
  // _01df: cmp ax, 003fh; jbe short _01e7; mov ax, 003fh
  // _01e7: mov fs:[si], al; inc si; jnz short _01cc
  for (size_t px_idx = 0; px_idx < 0x10000; px_idx++) {
    int16_t diff = (int16_t)height_buf[px_idx] -
                   (int16_t)height_buf[MAP_IDX_CAP(px_idx + 3)];
    diff += 0x20;
    slope_buf[px_idx] = CLAMP(diff, 0x0000, 0x003F);
  }

  // Original final smoothing: _01ef loop
  // xor si, si
  // _01ef: mov al, es:[si]; xor ah, ah; add al, es:[si + 01]; adc ah, 00
  // add al, es:[si + 0100h]; adc ah, 00; add al, es:[si + 0101h]; adc ah, 00
  // shr ax, 02; mov es:[si], al; inc si; jnz short _01ef
  for(int y = 0; y < 256; y++) {
    for(int x = 0; x < 256; x++) {
      size_t px_idx = MAP_IDX(x, y);
      int sum = height_buf[px_idx];
      sum += height_buf[MAP_IDX(x+1, y)];
      sum += height_buf[MAP_IDX(x, y+1)];
      sum += height_buf[MAP_IDX(x+1, y+1)];
      height_buf[px_idx] = sum / 4;
    }
  }
}

// Helper function equivalent to _cr02
uint16_t cr02_function(unsigned int terrain_idx, uint8_t frac_step) {
  // mov al, gs:[bx + 01]
  // sub al, gs:[bx]
  // imul ch
  // shl ax, 1
  // add ah, gs:[bx]
  uint8_t slope_delta =
      terra_data[MAP_IDX_CAP(terrain_idx + 1)] - terra_data[terrain_idx];
  uint16_t interp_color =
      (uint16_t)((int8_t)slope_delta * frac_step) * 2; // signed multiply
  interp_color += terra_data[terrain_idx] * 256;
  return interp_color;
}

// Subroutine for diamond point processing
// Original: _cr03
uint8_t cr03_subroutine(uint8_t *buffer, unsigned int src_idx,
                        unsigned int dst_idx,
                        uint8_t half_step, unsigned int ref_val) {
  // cmp byte ptr es:[di], 0ffh  ; check if point already processed
  // jnz short _0276  ; if not 0xFF, skip processing
  if (buffer[dst_idx] != 0xff) {
    // _0276: mov dl, es:[bx] ; ret  ; return existing value
    return buffer[src_idx];
  }

  // Point needs processing
  // xor dh, dh  ; dh = 0
  // mov dl, es:[bx]  ; dl = buffer[bx]
  // add dl, es:[bx] ; adc dh, 00  ; add same value (double it)
  // shr dx, 1  ; divide by 2 (average)
  uint16_t sum_word = (ref_val + buffer[src_idx]) / 2;

  // call _cr05  ; apply random variation
  int16_t varied_val = cr05_subroutine(
      (uint8_t)sum_word, half_step); // 1); // cl=1 for diamond points

  // Range checking same as main function
  // js short _0271
  // _0271: xor al, al
  // cmp ax, 00feh ; jbe short _0273
  // mov al, 0feh
  // _0273: mov es:[di], al  ; store result
  buffer[dst_idx] = CLAMP(varied_val, 0x0000, 0x00fe);

  // _0276: mov dl, es:[bx] ; ret  ; return the reference value
  return buffer[src_idx];
}

// Random number generator and variation application
// Original: _cr05
int cr05_subroutine(uint8_t center_val, int step_size) {
  // mov ch, dl  ; save input value
  // Linear congruential generator
  // mov ax, 00abh ; mul si  ; ax = 0x00ab * si
  uint32_t rng_acc = 0x00ab * var_035d_tmp;

  // add ax, 2bcdh ; adc dx, +00  ; add constant
  rng_acc += 0x2bcd;
  uint16_t rng_hi = (rng_acc >> 16); // high word
  rng_acc &= 0xFFFF;                 // keep low word

  // div word ptr cs:[_034d]  ; divide by constant
  uint32_t dividend = (rng_hi << 16) | rng_acc;
  uint16_t quotient = dividend / const_cf85;
  uint16_t remainder = dividend % const_cf85;

  // mov si, dx  ; update si with remainder (new seed)
  var_035d_tmp = remainder;

  // sub dx, 67c2h  ; subtract offset to center around 0
  int16_t rng_offset = remainder - 0x67c2;

  // mov al, cl ; xor ah, ah ; imul dx  ; multiply step size by random
  int32_t scaled_rand = (int32_t)step_size * rng_offset;

  // mov al, ah ; mov ah, dl  ; extract middle bytes
  uint8_t rand_byte = (scaled_rand >> 8) & 0xFF;
  uint8_t rand_hi_byte = (scaled_rand >> 16) & 0xFF;

  // sar ax, 05  ; arithmetic shift right by 5 (divide by 32)
  int16_t rand_variation = (int16_t)((rand_hi_byte << 8) | rand_byte);
  rand_variation >>= 5;

  // cbw  ; sign extend al to ax
  rand_variation &= 0x00FF; // zero high order byte
  if (rand_variation & 0x80) {
    rand_variation |= 0xFF00; // sign extend
  }

  // add al, ch ; adc ah, 00  ; add original value
  return rand_variation + center_val;
}

// Diamond-square algorithm - original _0219
void diamond_square(uint8_t *buffer, uint16_t full_size, int start_x,
                    int start_y) {
  // _0219: mov bp, sp
  // Function entry - parameters already in variables

  // mov bx, ss:[bp + 04]  ; bx = buffer offset (x parameter)
  // mov cx, ss:[bp + 02]  ; cx = size
  // shr cx, 1  ; cx = size / 2 (half step)
  uint16_t half_size = full_size / 2;
  uint16_t half_step = half_size; // lower byte of half_size
  unsigned int x = start_x;
  unsigned int y = start_y;
  unsigned int pos = MAP_IDX(x, y);

  // mov dl, es:[bx]  ; dl = buffer[bx] (center value)
  uint8_t corner_val = buffer[pos];
  const uint16_t step_twice = 2 * half_step;

  // Diamond step - calculate 4 diamond points

  // add bl, cl  ; move right by half step
  x = (x + half_step) % 256;
  pos = MAP_IDX(x, y);
  // mov di, bx
  unsigned int mid_pos = pos;
  // add bl, cl  ; move right again
  x = (x + half_step) % 256;
  pos = MAP_IDX(x, y);
  // call _cr03  ; process right point
  corner_val = cr03_subroutine(buffer, pos, mid_pos, half_step, corner_val);

  // add bh, cl  ; move down by half step
  y = (y + half_step) % 256;
  pos = MAP_IDX(x, y);
  // mov di, bx
  mid_pos = pos;
  // add bh, cl  ; move down again
  y = (y + half_step) % 256;
  pos = MAP_IDX(x, y);
  // call _cr03  ; process bottom point
  corner_val = cr03_subroutine(buffer, pos, mid_pos, half_step, corner_val);

  // sub bl, cl  ; move left by half step
  x = (x - half_step) % 256;
  pos = MAP_IDX(x, y);
  // mov di, bx
  mid_pos = pos;
  // sub bl, cl  ; move left again
  x = (x - half_step) % 256;
  pos = MAP_IDX(x, y);
  // call _cr03  ; process left point
  corner_val = cr03_subroutine(buffer, pos, mid_pos, half_step, corner_val);

  // sub bh, cl  ; move up by half step
  y = (y - half_step) % 256;
  pos = MAP_IDX(x, y);
  // mov di, bx
  mid_pos = pos;
  // sub bh, cl  ; move up again
  y = (y - half_step) % 256;
  pos = MAP_IDX(x, y);
  // call _cr03  ; process top point
  corner_val = cr03_subroutine(buffer, pos, mid_pos, half_step, corner_val);

  // Square step - calculate center value from 4 corner averages

  // xor dh, dh  ; dh = 0 (high byte for sum)
  uint16_t corner_sum = buffer[MAP_IDX(start_x, start_y)]; // Reset corner_val to original center value

  // add bl, cl ; add bl, cl  ; move to bottom-right corner
  x = (x + step_twice) % 256;
  pos = MAP_IDX(x, y);
  // add dl, es:[bx]  ; add bottom-right corner
  // adc dh, 00  ; add carry to high byte
  corner_sum += buffer[pos];

  // add bh, cl ; add bh, cl  ; move to bottom-right corner
  y = (y + step_twice) % 256;
  pos = MAP_IDX(x, y);
  // add dl, es:[bx]  ; add bottom-right value
  // adc dh, 00  ; add carry
  corner_sum += buffer[pos];

  // sub bl, cl ; sub bl, cl  ; move to bottom-left corner
  x = (x - step_twice) % 256;
  pos = MAP_IDX(x, y);
  // add dl, es:[bx]  ; add bottom-left value
  // adc dh, 00  ; add carry
  // Combine sum_carry and corner_val into 16-bit corner_sum
  corner_sum += buffer[pos];

  // shr dx, 02  ; divide sum by 4 (average of 4 corners)
  corner_sum >>= 2;

  // call _cr05  ; apply random variation
  int new_val = cr05_subroutine(corner_sum, half_step);
  new_val = CLAMP(new_val, 0x0000, 0x00fe);

  // _03cc: add bl, cl ; sub bh, cl  ; move to center position
  x = (x + half_step) % 256;
  y = (y - half_step) % 256;
  pos = MAP_IDX(x, y);

  // mov es:[bx], al  ; store result in center
  buffer[pos] = new_val;

  // Recursive calls for subdivisions

  // cmp cl, 01  ; if step size is 1, we're done
  // jz short _0407
  if (half_step == 1) {
    return;
  }

  // xor ch, ch  ; clear high byte of cx
  half_size = half_step; // half_size now contains just the step size

  // Recursive call 1: top-left quadrant
  // sub bl, cl ; sub bh, cl  ; move to top-left quadrant center
  x = (x - half_step) % 256;
  y = (y - half_step) % 256;
  // push bx ; push cx ; call _0219
  diamond_square(buffer, half_size, x, y);

  // Recursive call 2: top-right quadrant
  // add ss:[bp + 02], cl  ; adjust x coordinate right
  half_step = half_size & 0xFF;
  diamond_square(buffer, half_size, x + half_step, y);

  // Recursive call 3: bottom-right quadrant
  // add ss:[bp + 03], cl  ; adjust y coordinate down
  half_step = half_size & 0xFF;
  diamond_square(buffer, half_size, x + half_step,
                 y + half_step);

  // Recursive call 4: bottom-left quadrant
  // sub ss:[bp + 02], cl  ; adjust x coordinate back left
  half_step = half_size & 0xFF;
  diamond_square(buffer, half_size, x, y + half_step);
}

// Handle input - original _0408
void handle_input() {
  SDL_Event event;
  int mouse_dx = 0, mouse_dy = 0;

  while (SDL_PollEvent(&event)) {
    switch (event.type) {
    case SDL_QUIT:
    case SDL_KEYDOWN:
      key_pressed = 1;
      break;
    case SDL_MOUSEMOTION:
      mouse_dx = event.motion.xrel;
      mouse_dy = event.motion.yrel;
      break;
    }
  }

  // Update position based on mouse movement - matching original _0423
  var_0355 += mouse_dx; // add cs:[_0355], cx
  var_0357 -= mouse_dy; // sub cs:[_0357], dx

  // Calculate height at current position
  calculate_height();
}

// Calculate height at position - original _105c
void calculate_height() {
  // Original: mov fs, cs:[_0347]; ror cx, 04; ror dx, 04
  uint16_t player_x_rot = var_0355;
  uint16_t player_y_rot = var_0357;

  // Rotate right by 4 bits (equivalent to ror cx, 04)
  player_x_rot = (player_x_rot >> 4) | (player_x_rot << 12);
  player_y_rot = (player_y_rot >> 4) | (player_y_rot << 12);

  // Original: mov bl, cl; mov bh, dl; shr cx, 0ch; shr dx, 0ch
  unsigned int map_x = player_x_rot % 256;
  unsigned int map_y = player_y_rot % 256;
  unsigned int map_pos = MAP_IDX(map_x, map_y);
  player_x_rot >>= 12;
  player_y_rot >>= 12;

  // Original: inc bl; mov al, fs:[bx]; xor ah, ah; dec bl; sub al, fs:[bx]; sbb
  // ah, 00
  map_x++;
  unsigned int map_pos_right = MAP_IDX(map_x, map_y);
  int16_t height_delta = aux_array[map_pos_right] - aux_array[map_pos];
  map_x--;

  // Original: push dx; imul cx; mov dl, fs:[bx]; xor dh, dh; shl dx, 04; add
  // ax, dx; pop dx
  int32_t result = (int32_t)height_delta * (int16_t)player_x_rot;
  uint16_t base_height = aux_array[map_pos] << 4;
  result += base_height;
  uint16_t interp_x = (uint16_t)result;

  // Original: inc bh; inc bl; mov al, fs:[bx]; xor ah, ah; dec bl; sub al,
  // fs:[bx]; sbb ah, 00
  map_y++;
  map_x++;
  map_pos = MAP_IDX(map_x, map_y);
  map_pos_right = MAP_IDX(map_x - 1, map_y);
  height_delta = aux_array[map_pos] - aux_array[map_pos_right];

  // Original: push dx; imul cx; mov dl, fs:[bx]; xor dh, dh; shl dx, 04; add
  // ax, dx; pop dx
  result = (int32_t)height_delta * (int16_t)player_x_rot;
  base_height = aux_array[map_pos_right] << 4;
  result += base_height;
  uint16_t interp_x2 = (uint16_t)result;

  // Original: mov ax, di; sub ax, si; imul dx; shl si, 04; add ax, si
  result = ((int32_t)(interp_x2 - interp_x) * (int16_t)player_y_rot) +
           (interp_x << 4);
  var_0359 = (uint16_t)result;

  // add ah, 19h; jae short _0446; mov ax, 0ffffh
  uint32_t height_result = (var_0359 & 0xFF) | (((var_0359 >> 8) + 0x19) << 8);
  if (height_result >= 0x10000) {
    var_0359 = 0xffff;
  } else {
    var_0359 = height_result;
  }
}

// Render landscape - original _0459
void render_sky() {
  // Original: mov di, 0; mov ecx, 00000063h
  size_t dst_idx = 0;
  const size_t render_px_count =
      (size_t)256 * (size_t)200;

  // Original: xor eax, eax; mov ax, cs:[_0359]; neg ax; shr ax, 03; add ax,
  // 4000h; shl eax, 0dh
  // neg ax
  int32_t sky_offset = (uint16_t)(-(int16_t)var_0359);
  sky_offset += 0x20000;
  sky_offset <<= 10;
  int32_t sky_y_step = sky_offset;

  // Original: xor eax, eax; mov ax, cs:[_0355]; shl eax, 09
  sky_offset = var_0355;
  sky_offset <<= 9;
  int32_t sky_x_start = sky_offset;

  // Original: xor eax, eax; mov ax, cs:[_0357]; shl eax, 09
  sky_offset = var_0357;
  sky_offset <<= 9;
  int32_t sky_y_start = sky_offset;

  // Original sky rendering loop: _049d
  for (int32_t scan_line = 99; scan_line > 0; scan_line--) {
    // Original: mov eax, cs:[_tmp_360_dd]; xor edx, edx; div ecx
    uint32_t step_per_col = sky_y_step / scan_line;

    // Original: mov esi, cs:[_tmp_364_dd]; mov ebp, cs:[_tmp_368_dd]
    // sub esi, eax; add ebp, eax
    uint32_t sky_x_pos = sky_x_start - step_per_col;
    uint32_t sky_y_pos = sky_y_start + step_per_col;

    // Original: shr eax, 07; mov bx, si; shr esi, 10h; and si, 00ffh
    step_per_col >>= 7;
    uint16_t sky_frac_x = sky_x_pos % 0x10000;
    uint16_t sky_tex_idx = (sky_x_pos >> 16) % 256;

    // Original: shr ebp, 08; and bp, 0ff00h; or si, bp
    uint16_t sky_tex_y_part = ((sky_y_pos >> 16) % 256) * 256;
    sky_tex_idx |= sky_tex_y_part;

    // Original: mov ebp, eax; shr ebp, 10h; dec bp
    uint16_t col_y_step = (step_per_col >> 16) - 1;

    // Original: movsb; push cx; mov cx, 00ffh
    uint8_t sky_color = sky_data[sky_tex_idx++];
    if (dst_idx < render_px_count) {
      landscape_data[dst_idx] = sky_color;
    }
    dst_idx++;

    // Original: _04df: add bx, ax; adc si, bp; movsb; loop _04df
    for (int32_t col_counter = 256 - 1; col_counter > 0;
         col_counter--) {
      /*
      looks like 2 32-bit ints are formed:
      - A: [sky_tex_idx:sky_frac_x]
      - S: [col_y_step:(step_per_col & 0xFFFF)]
      and we are applying: A <- A + S
      */
      uint32_t frac_plus_step = sky_frac_x + (step_per_col & 0xFFFF);
      sky_frac_x = frac_plus_step & 0xFFFF;
      sky_tex_idx += col_y_step + (frac_plus_step > 0xFFFF); // add carry
      sky_color = sky_data[sky_tex_idx++];
      if (dst_idx < render_px_count) {
        landscape_data[dst_idx] = sky_color;
      }
      dst_idx++;
    }
  }

  // Original horizon line: mov ax, 5050h; mov cx, 0080h; rep stosw
  for (int i = 0; i < 256; i++) {
    if (dst_idx < render_px_count) {
      landscape_data[dst_idx] = 80;
    }
    dst_idx++;
  }

  // Original ground rendering: _09f5 loop
  uint16_t ground_step = (var_0359/2) + 10;
  for (int32_t band = 4; band < (4 + 40); band++) {
    // Original: mov ax, si; xor dx, dx; div bx; shr ax, 07
    uint16_t shade = ground_step / band;
    shade /= 128;

    // Original: cmp ax, 003fh; jbe short _0a05; mov al, 3fh
    if (shade > 63)
      shade = 63;

    // Original: mov ah, al; mov dl, al; shl ax, 10h; mov al, dl; mov ah, al
    uint8_t color = (uint8_t)shade;

    // Original: mov cx, 80h; rep stosw
    for (int x = 0; x < 256; x++) {
      if (dst_idx < render_px_count) {
        landscape_data[dst_idx] = color;
      }
      dst_idx++;
    }
  }
}

/**
 * Renders the 3D terrain.
 * This is the most complex part of the original demo. It's a form of
 * voxel-space or ray-casting rendering.
 * Corresponds to `_0a1f`.
 */
void render_terrain() {
  // Initialize segment pointers and clear buffers
  // mov eax, 7d007d00h; mov di, 03aah; mov cx, 0080h; rep stosd
  for (int i = 0; i < 256; i++) {
    g_terrain_column_heights[i] = 0x7d00;
  }

  // xor eax, eax; mov di, 05aah; mov cx, 0080h; rep stosd
  for (int i = 0; i < 256; i++) {
    g_terrain_column_colors[i] = 0;
  }

  // mov word ptr cs:[_tmp_3a0], 0078h
  int16_t ray_row = 60;
  if (ray_row < 1) {
    ray_row = 1;
  }

  // Main processing loop (_0a48)
  while (ray_row > 0) {
    // mov si, cs:[_tmp_3a0]; mov si, ds:[si + 0190h]; shl si, 04
    uint16_t step_size =
        g_terrain_ray_table[ray_row + 200];
    step_size <<= 4;

    // mov ax, cs:[_0357]; and ax, 000fh; xor al, 0fh; add si, ax
    uint16_t y_frac = var_0357 & 0x0f;
    y_frac ^= 0x0f;
    step_size += y_frac;

    // mov ax, cs:[_0359]; xor dx, dx; div si; add ax, 0064h; mov cs:[_tmp_3a2],
    // ax
    uint32_t temp_div = var_0359;
    int16_t tmp_03a2 = (int16_t)(temp_div / step_size) +
                       (200 / 2);

    // xor eax, eax; mov ax, si; shl eax, 06; mov ds:[03a6h], eax
    uint32_t step_scaled = step_size << 6;

    // Conditional branch logic
    uint16_t tmp_03a4 = 0;
    if (ray_row == 1) {
      // mov word ptr cs:[_tmp_3a2], 7d00h; mov word ptr cs:[_tmp_3a4], 0000
      tmp_03a2 = 0x7d00; // or 0x7556 based on comment
    } else {
      // xor ax, ax; mov dx, 0001; div si; mov cs:[_tmp_3a4], ax
      temp_div = 0x10000; // dx=1, ax=0
      tmp_03a4 = temp_div / step_size;
    }

    // Complex calculation section (_0a96)
    // xor ecx, ecx; mov cx, cs:[_0355]; shl ecx, 0ch
    uint32_t tex_x_pos = var_0355 << 12;

    // mov eax, ds:[03a6h]; shl eax, 07; sub ecx, eax
    tex_x_pos -= (step_scaled << 7);

    // mov dx, cs:[_0357]; shl dx, 04; mov ebx, ecx; shr ebx, 10h; mov bh, dh
    uint16_t map_y_frac = var_0357 << 4;

    uint32_t terrain_pos = tex_x_pos >> 16;

    terrain_pos = (terrain_pos & 0xFFFF00FF) | (map_y_frac & 0xFF00);

    // mov ax, si; shr ax, 04; add bh, al; shr cx, 1
    uint16_t sub_step = step_size >> 4;
    // add bh, al
    uint8_t tmp_add_bh_al =
        ((terrain_pos & 0x0000FF00) >> 8) + (sub_step & 0xFF);
    terrain_pos = (terrain_pos & 0xFFFF00FF) | ((uint32_t)(tmp_add_bh_al) << 8);

    uint16_t tex_x_frac = (tex_x_pos & 0xFFFF) >> 1;

    // mov si, 01feh; mov word ptr cs:[_tmp_364_dw], 0
    int16_t src_idx = (int16_t)256 - 1;

    uint16_t base_x = 0;

    // Inner rendering loop (_0ace)
    while (src_idx >= 0) {
      // shl cx, 1; add cx, ds:[03a6h]; adc bl, ds:[03a8h]; shr cx, 1
      tex_x_frac <<= 1;
      uint32_t temp_add_cx = (uint32_t)tex_x_frac + (step_scaled & 0xFFFF);
      uint8_t temp_bl = terrain_pos & 0xFF;
      temp_bl += (step_scaled >> 16) & 0xFF;
      if (temp_add_cx > 0xFFFF) {
        temp_bl += 1; // carry to bl
      }
      terrain_pos = (terrain_pos & 0xFFFFFF00) | temp_bl;
      tex_x_frac = temp_add_cx & 0xFFFF;
      tex_x_frac >>= 1;

      uint16_t terrain_idx = MAP_IDX_CAP(terrain_pos);

      // mov al, fs:[bx + 01]; xor ah, ah; sub al, fs:[bx]; sbb ah, 00; imul cx
      uint8_t height_delta = aux_array[MAP_IDX_CAP(terrain_idx + 1)];
      uint16_t height_val = height_delta;

      height_val -= aux_array[terrain_idx];
      height_val = (height_val & 0xFF) | ((height_val & 0xFF00) ? 0xFF00 : 0);

      // Signed multiply
      int32_t temp_result = (int16_t)height_val * (int16_t)tex_x_frac;

      // shrd ax, dx, 07; add ah, fs:[bx]
      uint16_t height_sample = temp_result & 0xFFFF;
      // temp_result = ((uint32_t)dx << 16) | height_sample;
      height_sample = (temp_result >> 7) & 0xFFFF;
      height_sample =
          (height_sample & 0xFF) |
          ((((height_sample >> 8) + aux_array[terrain_idx]) << 8) & 0xFF00);

      // mul word ptr cs:[_tmp_3a4]; mov di, cs:[_tmp_3a2]; sub di, dx
      temp_result = (uint32_t)height_sample * tmp_03a4;

      // Clamp di value
      int16_t base_y = tmp_03a2 - (temp_result >> 16);
      base_y = CLAMP(base_y, -1, 200 - 1);

      // mov bp, ds:[si + 03aah]; mov ds:[si + 03aah], di; sub bp, di
      int16_t height_span = g_terrain_column_heights[src_idx] - base_y;
      g_terrain_column_heights[src_idx] = base_y;

      if (height_span < 0) {
        // Positive branch - call cr02 and update buffer
        // shl di, 08; add di, word ptr cs:[_tmp_364_dw]; call _cr02
        uint16_t cr02_res = cr02_function(
            terrain_idx, tex_x_frac / 256); // ch = high byte of cx

        // Complex buffer update logic
        uint16_t prev_color = g_terrain_column_colors[src_idx];
        g_terrain_column_colors[src_idx] = cr02_res;

        int16_t color_step =
            (int16_t)(prev_color - cr02_res) / height_span; // signed division
        uint16_t color = prev_color;

        // Pixel drawing loop (derived from jump table logic)
        uint16_t table_offset = g_terrain_ray_table
            [(uint16_t)(height_span + 200)];
        int index_row_offset =
            (table_offset - 0xb3e) / 6 - (200 - 1);

        for (; index_row_offset != 1; index_row_offset++) {
          int idx = (base_y + index_row_offset) * 256 + base_x;
          if (idx < 256 * 200)
            landscape_data[idx] = color / 256;
          color += color_step;
        }
      } else {
        // Negative branch - simpler update
        g_terrain_column_colors[src_idx] =
            cr02_function(terrain_idx, tex_x_frac / 256);
      }
      base_x++;
      src_idx--;
    }

    // End of inner loop processing
    ray_row -= 1;

    if (ray_row >= 0) {
      // mov si, cs:[_tmp_3a0]; mov si, ds:[si + 0190h]; test si, 0003
      step_size = g_terrain_ray_table[ray_row + 200];
      if ((step_size & 3) == 0) { // Or si % 4 == 0
        step_size >>= 2;          // shr si, 02
      }
    }
  }
}

int main(int argc, char *argv[]) {
  // Initialize SDL
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    printf("SDL initialization failed: %s\n", SDL_GetError());
    return 1;
  }

  // Create window
  window = SDL_CreateWindow("MARS Landscape Renderer", SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED,
                            320 * 2, // Scale 2x for visibility
                            200 * 2, SDL_WINDOW_SHOWN);

  if (!window) {
    printf("Window creation failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  // Create renderer
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer) {
    printf("Renderer creation failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  // Create texture for screen buffer
  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                              SDL_TEXTUREACCESS_STREAMING, 320,
                              200);

  if (!texture) {
    printf("Texture creation failed: %s\n", SDL_GetError());
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  // Initialize memory and data structures
  init_memory_segments();

  init_data();

  // Initialize random seed with time - matching original int 1ah
  time_t current_time = 0;
  time(&current_time);
  var_035d = (uint16_t)(current_time & 0x7FFF);
  random_seed = var_035d;

  // Load palette
  load_palette();

  // Prepare sky and terrain (equivalent to call _00f3 and call _0153)
  prepare_sky();
  prepare_terrain();

  // Initialize position
  var_0355 = 0x03e8; // mov word ptr cs:[_0351], 03e8h
  var_0357 = 0x03e8; // mov word ptr cs:[_0353], 03e8h

  // Enable relative mouse mode
  SDL_SetRelativeMouseMode(SDL_TRUE);

  Uint32 start_ticks = SDL_GetTicks();
  int frame_count = 0;

  // Main loop - original _00bc
  while (!key_pressed) {
    // Handle input
    handle_input();

    // Render landscape
    render_sky();

    // Render terrain
    render_terrain();

    // Convert 8-bit indexed to RGBA
    uint32_t *pixels;
    int pitch;

    if (SDL_LockTexture(texture, NULL, (void **)&pixels, &pitch) == 0) {
      int x_offset = (320 - 256) / 2;

      for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 256; x++) {
          uint8_t color_index = landscape_data[y * 256 + x];
          pixels[y * (pitch / 4) + x + x_offset] = palette[color_index];
        }
      }
      SDL_UnlockTexture(texture);
    }

    // Render to screen
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    // Small delay to prevent excessive CPU usage
    SDL_Delay(16); // ~60 FPS
    ++frame_count;
  }

  Uint32 finish_ticks = SDL_GetTicks();
  double exec_time = (double)(finish_ticks - start_ticks) / 1000.;
  printf("Time elapsed: %.6f\n", exec_time);
  printf("%.2f FPS\n", frame_count / exec_time);

  // Cleanup
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  cleanup_memory_segments();
  SDL_Quit();

  return 0;
}
