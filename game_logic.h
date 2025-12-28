/*
 * Hyperspace - Common Game Logic Header
 * This file contains all game logic shared between platforms
 */

#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

// Memory position for mesh decoding
static int mem_pos = 0;

// Read a raw byte from map memory and convert to signed value * 0.5
static fix16_t decode_byte(void) {
    int res = map_memory[mem_pos];
    mem_pos++;
    if (res >= 128) res = res - 256;
    return fix16_from_int(res) >> 1;
}

static int decode_byte_int(void) {
    int res = map_memory[mem_pos];
    mem_pos++;
    if (res >= 128) res = res - 256;
    return res / 2;
}

static void decode_mesh(Mesh* mesh, fix16_t scale) {
    int nb_vert = decode_byte_int();
    if (nb_vert < 0) nb_vert = 0;
    if (nb_vert > 256) nb_vert = 256;
    mesh->num_vertices = nb_vert;
    mesh->vertices = (Vec3*)calloc(nb_vert > 0 ? nb_vert : 1, sizeof(Vec3));
    mesh->projected = (Vec3*)calloc(nb_vert > 0 ? nb_vert : 1, sizeof(Vec3));

    for (int i = 0; i < nb_vert; i++) {
        mesh->vertices[i].x = fix16_mul(decode_byte(), scale);
        mesh->vertices[i].y = fix16_mul(decode_byte(), scale);
        mesh->vertices[i].z = fix16_mul(decode_byte(), scale);
    }

    int nb_tri = decode_byte_int();
    if (nb_tri < 0) nb_tri = 0;
    if (nb_tri > 256) nb_tri = 256;
    mesh->num_triangles = nb_tri;
    mesh->triangles = (Triangle*)calloc(nb_tri > 0 ? nb_tri : 1, sizeof(Triangle));

    for (int i = 0; i < nb_tri; i++) {
        Triangle* tri = &mesh->triangles[i];
        tri->tri[0] = decode_byte_int() - 1;
        tri->normal.x = fix16_div(decode_byte(), F16(63.5));
        tri->uv[0][0] = decode_byte();
        tri->uv[0][1] = decode_byte();

        tri->tri[1] = decode_byte_int() - 1;
        tri->normal.y = fix16_div(decode_byte(), F16(63.5));
        tri->uv[1][0] = decode_byte();
        tri->uv[1][1] = decode_byte();

        tri->tri[2] = decode_byte_int() - 1;
        tri->normal.z = fix16_div(decode_byte(), F16(63.5));
        tri->uv[2][0] = decode_byte();
        tri->uv[2][1] = decode_byte();
    }
}

static void transform_pos(Vec3* proj, const Mat34* mat, const Vec3* pos) {
    mat_mul_pos(proj, mat, pos);
    fix16_t c = fix16_div(FIX_PROJ_CONST, proj->z);
    proj->x = FIX_SCREEN_CENTER + fix16_mul(proj->x, c);
    proj->y = FIX_SCREEN_CENTER - fix16_mul(proj->y, c);
    if (c > 0 && c <= F16(10.0)) {
        proj->z = c;
    } else {
        proj->z = 0;
    }
}

static void rasterize_flat_tri(Vec3* v0, Vec3* v1, Vec3* v2,
                                fix16_t* uv0, fix16_t* uv1, fix16_t* uv2, fix16_t light) {
    fix16_t y0 = v0->y;
    fix16_t y1 = v1->y;
    fix16_t firstline, lastline;

    if (y0 < y1) {
        firstline = fix16_floor(y0 + FIX_HALF) + FIX_HALF;
        lastline = fix16_floor(y1 - FIX_HALF) + FIX_HALF;
    } else if (y0 == y1) {
        return;
    } else {
        firstline = fix16_floor(y1 + FIX_HALF) + FIX_HALF;
        lastline = fix16_floor(y0 - FIX_HALF) + FIX_HALF;
    }

    if (firstline < FIX_HALF) firstline = FIX_HALF;
    if (lastline > F16(SCREEN_HEIGHT - 0.5)) lastline = F16(SCREEN_HEIGHT - 0.5);

    fix16_t x0 = v0->x, z0 = v0->z;
    fix16_t x1 = v1->x, z1 = v1->z;
    fix16_t x2 = v2->x, y2 = v2->y, z2 = v2->z;

    fix16_t uv0x = uv0[0], uv0y = uv0[1];
    fix16_t uv1x = uv1[0], uv1y = uv1[1];
    fix16_t uv2x = uv2[0], uv2y = uv2[1];

    fix16_t cb0 = fix16_mul(x1, y2) - fix16_mul(x2, y1);
    fix16_t cb1 = fix16_mul(x2, y0) - fix16_mul(x0, y2);
    fix16_t d = cb0 + cb1 + fix16_mul(x0, y1) - fix16_mul(x1, y0);
    if (fix16_abs(d) < F16(0.001)) return;

    fix16_t dy = y1 - y0;
    if (fix16_abs(dy) < F16(0.001)) return;
    fix16_t invdy = fix16_div(fix16_one, dy);

    int tex_x = cur_tex->x;
    int tex_y = cur_tex->y;
    int tex_lit_x = cur_tex->light_x;

    for (fix16_t y = firstline; y <= lastline; y += fix16_one) {
        fix16_t coef = fix16_mul(y - y0, invdy);
        fix16_t xfirst = fix16_floor(x0 + fix16_mul(coef, x1 - x0) + F16(0.48)) + FIX_HALF;
        fix16_t xlast = fix16_floor(x0 + fix16_mul(coef, x2 - x0) - F16(0.48)) + FIX_HALF;

        if (xfirst < FIX_HALF) xfirst = FIX_HALF;
        if (xlast > F16(SCREEN_WIDTH - 0.5)) xlast = F16(SCREEN_WIDTH - 0.5);

        fix16_t x0y = fix16_mul(x0, y);
        fix16_t x1y = fix16_mul(x1, y);
        fix16_t x2y = fix16_mul(x2, y);

        for (fix16_t x = xfirst; x <= xlast; x += fix16_one) {
            fix16_t b0 = fix16_div(cb0 + fix16_mul(x, y1) + x2y - fix16_mul(x, y2) - x1y, d);
            fix16_t b1 = fix16_div(cb1 + fix16_mul(x, y2) + x0y - fix16_mul(x, y0) - x2y, d);
            fix16_t b2 = fix16_one - b0 - b1;

            b0 = fix16_mul(b0, z0);
            b1 = fix16_mul(b1, z1);
            b2 = fix16_mul(b2, z2);

            fix16_t d2 = b0 + b1 + b2;
            if (fix16_abs(d2) < F16(0.001)) continue;

            fix16_t uvx = fix16_div(fix16_mul(b0, uv0x) + fix16_mul(b1, uv1x) + fix16_mul(b2, uv2x), d2);
            fix16_t uvy = fix16_div(fix16_mul(b0, uv0y) + fix16_mul(b1, uv1y) + fix16_mul(b2, uv2y), d2);

            int offset_x = tex_x;
            int px = fix16_to_int(x);
            int py = fix16_to_int(y);
            int dither_val = sget(px % 8, 56 + py % 8);
            if (light <= F16(7.0) + fix16_mul(fix16_from_int(dither_val), F16(0.125))) {
                offset_x += tex_lit_x;
            }

            pset(px, py, sget(fix16_to_int(uvx) + offset_x, fix16_to_int(uvy) + tex_y));
        }
    }
}

static void rasterize_tri(int index, Triangle* tris, Vec3* projs) {
    Triangle* tri = &tris[index];
    if (tri->tri[0] < 0 || tri->tri[1] < 0 || tri->tri[2] < 0) return;
    if (cur_tex == NULL) return;

    Vec3* v0 = &projs[tri->tri[0]];
    Vec3* v1 = &projs[tri->tri[1]];
    Vec3* v2 = &projs[tri->tri[2]];

    fix16_t x0 = v0->x, y0 = v0->y;
    fix16_t x1 = v1->x, y1 = v1->y;
    fix16_t x2 = v2->x, y2 = v2->y;

    fix16_t nz = fix16_mul(x1 - x0, y2 - y0) - fix16_mul(y1 - y0, x2 - x0);
    if (nz < 0) return;

    fix16_t* uv0 = tri->uv[0];
    fix16_t* uv1 = tri->uv[1];
    fix16_t* uv2 = tri->uv[2];

    Vec3 *tv0 = v0, *tv1 = v1, *tv2 = v2;
    fix16_t *tuv0 = uv0, *tuv1 = uv1, *tuv2 = uv2;

    if (tv1->y < tv0->y) { Vec3* t = tv1; tv1 = tv0; tv0 = t; fix16_t* tu = tuv1; tuv1 = tuv0; tuv0 = tu; }
    if (tv2->y < tv0->y) { Vec3* t = tv2; tv2 = tv0; tv0 = t; fix16_t* tu = tuv2; tuv2 = tuv0; tuv0 = tu; }
    if (tv2->y < tv1->y) { Vec3* t = tv2; tv2 = tv1; tv1 = t; fix16_t* tu = tuv2; tuv2 = tuv1; tuv1 = tu; }

    y0 = tv0->y; y1 = tv1->y; y2 = tv2->y;
    x0 = tv0->x;
    fix16_t z0 = tv0->z, z2 = tv2->z;

    if (y0 == y2) return;

    fix16_t light = fix16_mul(F16(15.0), vec3_dot(t_light_dir, &tri->normal));

    fix16_t c = fix16_div(y1 - y0, y2 - y0);
    Vec3 v3 = {x0 + fix16_mul(c, tv2->x - x0), y1, z0 + fix16_mul(c, z2 - z0)};

    fix16_t b0 = fix16_mul(fix16_one - c, z0);
    fix16_t b1 = fix16_mul(c, z2);
    fix16_t sum = b0 + b1;
    fix16_t invd = (sum > F16(0.001)) ? fix16_div(fix16_one, sum) : 0;

    fix16_t uv3[2] = {
        fix16_mul(fix16_mul(b0, tuv0[0]) + fix16_mul(b1, tuv2[0]), invd),
        fix16_mul(fix16_mul(b0, tuv0[1]) + fix16_mul(b1, tuv2[1]), invd)
    };

    if (tv1->x <= v3.x) {
        rasterize_flat_tri(tv0, tv1, &v3, tuv0, tuv1, uv3, light);
        rasterize_flat_tri(tv2, tv1, &v3, tuv2, tuv1, uv3, light);
    } else {
        rasterize_flat_tri(tv0, &v3, tv1, tuv0, uv3, tuv1, light);
        rasterize_flat_tri(tv2, &v3, tv1, tuv2, uv3, tuv1, light);
    }
}

static int tri_compare(const void* a, const void* b) {
    const Triangle* ta = (const Triangle*)a;
    const Triangle* tb = (const Triangle*)b;
    if (ta->z < tb->z) return -1;
    if (ta->z > tb->z) return 1;
    return 0;
}

static void sort_tris(Triangle* tris, int num, Vec3* projs) {
    for (int i = 0; i < num; i++) {
        Triangle* tri = &tris[i];
        tri->z = projs[tri->tri[0]].z + projs[tri->tri[1]].z + projs[tri->tri[2]].z;
    }
    qsort(tris, num, sizeof(Triangle), tri_compare);
}

static void real_init_ship(void) {
    mem_pos = 0;
    decode_mesh(&ship_mesh, fix16_one);
    ship_tex.x = 0;
    ship_tex.y = 64;
    ship_tex.light_x = 16;
    ship_tex_laser_lit.x = 32;
    ship_tex_laser_lit.y = 64;
    ship_tex_laser_lit.light_x = 16;
}

static void real_init_nme(void) {
    for (int i = 0; i < 4; i++) {
        decode_mesh(&nme_meshes[i], nme_scale[i]);
        nme_tex[i].x = 48 + i * 8;
        nme_tex[i].y = 64;
        nme_tex[i].light_x = 8;
    }
    nme_tex_hit.x = 80;
    nme_tex_hit.y = 64;
    nme_tex_hit.light_x = 8;
}

static void init_single_trail(Trail* trail, fix16_t z) {
    trail->pos0.z = z;
    trail->pos1.z = z - F16(2.0);
    trail->col = 1;
}

static void real_init_trail(void) {
    for (int i = 0; i < MAX_TRAILS; i++) {
        init_single_trail(&trails[i], sym_random_fix(F16(150.0)));
    }
}

static void init_single_bg(Background* bg, fix16_t z) {
    fix16_t a = rnd_fix(fix16_one);
    fix16_t r = F16(150.0) + rnd_fix(F16(150.0));
    fix16_t angle = fix16_mul(a, FIX_TWO_PI);
    vec3_set(&bg->pos, fix16_mul(r, fix16_cos(angle)), fix16_mul(r, -fix16_sin(angle)), z);
    bg->spd = F16(0.05) + rnd_fix(F16(0.05));
    if (flr_fix(rnd_fix(F16(6.0))) == 0) {
        bg->index = 8 + (int)rnd_fix(F16(8.0));
    } else {
        bg->index = -bg_color[get_random_idx(3)];
    }
}

static void real_init_bg(void) {
    for (int i = 0; i < MAX_BGS; i++) {
        init_single_bg(&bgs[i], sym_random_fix(F16(400.0)));
    }
}

static void real_init_main(void) {
    save_cart_data();
    cur_mode = 0;
    cam_angle_z = F16(-0.4);
    cam_angle_x = fix16_mul(fix16_from_int(flr_fix(rnd_fix(FIX_TWO)) * 2 - 1), F16(0.03) + rnd_fix(F16(0.1)));

    ship_x = 0;
    ship_y = 0;
    cam_x = 0;
    cam_y = 0;
    ship_spd_x = 0;
    ship_spd_y = 0;
    life = 4;
    hit_t = -1;
    barrel_cur_t = F16(-1.0);
    laser_on = false;
    global_t = 0;
    game_spd = fix16_one;
    cur_nme_t = 0;
    cur_sequencer_x = 96;
    cur_sequencer_y = 96;
    next_sequencer_t = 0;
    num_lasers = 0;
    num_nme_lasers = 0;
    num_enemies = 0;
    nb_nme_ship = 0;
    waiting_nme_clear = false;
    spawn_asteroids = false;
    fade_ratio = F16(-1.0);
    roll_angle = 0;
    roll_spd = 0;
    roll_f = 0;
    pitch_angle = 0;
    pitch_spd = 0;
    pitch_f = 0;
    cur_thrust = 0;
    aim_z = F16(-200.0);
    tgt_pos = 0;
    aim_life_ratio = F16(-1.0);
    old_noise_roll = 0;
    cur_noise_roll = 0;
    old_noise_pitch = 0;
    cur_noise_pitch = 0;
    cur_noise_t = 0;
    tgt_noise_t = 0;
    asteroid_mul_t = fix16_one;
    best_score = dget(0);
}

static Laser* spawn_laser(Laser* lasers_arr, int* count, Vec3 pos) {
    if (*count >= MAX_LASERS) return NULL;
    Laser* laser = &lasers_arr[*count];
    vec3_copy(&laser->pos0, &pos);
    vec3_copy(&laser->pos1, &pos);
    (*count)++;
    return laser;
}

static void remove_laser(Laser* lasers_arr, int* count, int idx) {
    if (idx < 0 || idx >= *count) return;
    for (int i = idx; i < *count - 1; i++) {
        lasers_arr[i] = lasers_arr[i + 1];
    }
    (*count)--;
}

static Enemy* spawn_nme(int type, Vec3 pos) {
    if (num_enemies >= MAX_ENEMIES) return NULL;
    Enemy* nme = &enemies[num_enemies];
    vec3_copy(&nme->pos, &pos);
    nme->type = type;
    nme->proj = nme_meshes[type - 1].projected;
    nme->life = nme_life[type - 1];
    nme->hit_t = -1;
    nme->rot_x = 0;
    nme->rot_y = 0;
    num_enemies++;
    return nme;
}

static void spawn_nme_ship(int type) {
    nb_nme_ship++;
    next_sequencer_t = global_t + F16(0.25);
    fix16_t desc_bounds = fix16_mul(nme_bounds[type - 2], FIX_TWO);
    Vec3 pos = {
        mid_fix(F16(-100.0), sym_random_fix(F16(50.0)) + ship_x, F16(100.0)),
        mid_fix(F16(-100.0), sym_random_fix(F16(50.0)) + ship_y, F16(100.0)),
        desc_bounds - F16(200.0)
    };
    Enemy* nme = spawn_nme(type, pos);
    if (nme) {
        vec3_set(&nme->spd, 0, 0, F16(8.0));
        vec3_copy(&nme->waypoint, &nme->pos);
        nme->waypoint.z = desc_bounds;
        nme->rot_x_spd = 0;
        nme->rot_y_spd = 0;
        nme->laser_t = 0;
        nme->stop_laser_t = 0;
        nme->next_laser_t = 0;
    }
}

static void hit_ship(Vec3* pos, fix16_t sqr_size) {
    fix16_t dx = pos->x - ship_x;
    fix16_t dy = pos->y - ship_y;
    fix16_t sqr_dist = fix16_mul(dx, dx) + fix16_mul(dy, dy);
    if (sqr_dist < sqr_size && hit_t == -1) {
        life--;
        hit_t = 0;
        vec3_copy(&hit_pos, pos);
        sfx(2, 1);
    }
}

#endif // GAME_LOGIC_H
