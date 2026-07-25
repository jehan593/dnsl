/* Regenerates data/icons/dnsl-{enabled,disabled}-*.png: a padlock glyph on a rounded Nord0
 * square, in two color variants — Nord8 (protected) and Nord3 (not protected) — mirroring dnsw's
 * own Assets/generate-icon.ps1. Re-run if the palette/motif changes; don't hand-edit the PNGs.
 * Usage: ./generate_icon <out_dir>
 */
#define _USE_MATH_DEFINES
#include <math.h>
#include <cairo/cairo.h>
#include <stdio.h>

#define NORD0_R (0x2E / 255.0)
#define NORD0_G (0x34 / 255.0)
#define NORD0_B (0x40 / 255.0)

#define NORD8_R (0x88 / 255.0)
#define NORD8_G (0xC0 / 255.0)
#define NORD8_B (0xD0 / 255.0)

#define NORD3_R (0x4C / 255.0)
#define NORD3_G (0x56 / 255.0)
#define NORD3_B (0x6A / 255.0)

static void draw_icon(cairo_t *cr, double size, double r, double g, double b)
{
    double corner = size * 0.22;

    /* Background: rounded square, Nord0 */
    cairo_new_sub_path(cr);
    cairo_arc(cr, size - corner, corner, corner, -M_PI / 2, 0);
    cairo_arc(cr, size - corner, size - corner, corner, 0, M_PI / 2);
    cairo_arc(cr, corner, size - corner, corner, M_PI / 2, M_PI);
    cairo_arc(cr, corner, corner, corner, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, NORD0_R, NORD0_G, NORD0_B);
    cairo_fill(cr);

    /* Foreground: padlock — shackle arc + body rect, accent color */
    double body_w = size * 0.46, body_h = size * 0.34;
    double body_x = (size - body_w) / 2.0, body_y = size * 0.52;
    double body_r = size * 0.05;
    double shackle_r = size * 0.16;
    double shackle_cx = size * 0.5, shackle_cy = body_y;
    double stroke_w = size * 0.075;

    cairo_set_source_rgb(cr, r, g, b);

    cairo_set_line_width(cr, stroke_w);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_new_sub_path(cr);
    cairo_arc(cr, shackle_cx, shackle_cy, shackle_r, M_PI, 2 * M_PI);
    cairo_stroke(cr);

    cairo_new_sub_path(cr);
    cairo_arc(cr, body_x + body_w - body_r, body_y + body_r, body_r, -M_PI / 2, 0);
    cairo_arc(cr, body_x + body_w - body_r, body_y + body_h - body_r, body_r, 0, M_PI / 2);
    cairo_arc(cr, body_x + body_r, body_y + body_h - body_r, body_r, M_PI / 2, M_PI);
    cairo_arc(cr, body_x + body_r, body_y + body_r, body_r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);

    /* Keyhole: a small Nord0 dot cut into the body */
    cairo_set_source_rgb(cr, NORD0_R, NORD0_G, NORD0_B);
    cairo_arc(cr, size * 0.5, body_y + body_h * 0.45, size * 0.035, 0, 2 * M_PI);
    cairo_fill(cr);
}

static int render(const char *out_dir, const char *variant, int size, double r, double g, double b)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);
    draw_icon(cr, (double)size, r, g, b);

    char path[512];
    snprintf(path, sizeof(path), "%s/dnsl-%s-%d.png", out_dir, variant, size);
    cairo_status_t status = cairo_surface_write_to_png(surface, path);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "failed to write %s: %s\n", path, cairo_status_to_string(status));
        return 1;
    }
    printf("wrote %s\n", path);
    return 0;
}

int main(int argc, char **argv)
{
    const char *out_dir = argc > 1 ? argv[1] : ".";
    int sizes[] = { 16, 32, 48, 256 };
    int rc = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        rc |= render(out_dir, "enabled", sizes[i], NORD8_R, NORD8_G, NORD8_B);
        rc |= render(out_dir, "disabled", sizes[i], NORD3_R, NORD3_G, NORD3_B);
    }
    return rc;
}
