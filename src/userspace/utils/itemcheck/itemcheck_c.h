#ifndef ITEMCHECK_C_H
#define ITEMCHECK_C_H

/* itemcheck's part written in C (from_c.c). */

#ifdef __cplusplus
extern "C" {
#endif

/* painter::gui's C face held: 1 when every step was shown and nothing
 * refused; the heading measured beforehand into *measured_width x
 * *measured_height points. */
int ItemcheckFromC(float *measured_width, float *measured_height);

#ifdef __cplusplus
}
#endif

#endif
