/* ===================================================================================
 * PackBits row decoder for RV32EC, hand written.
 *
 *   void map_row_rle(const uint8_t *p, const uint8_t *end, uint8_t *out, uint8_t w)
 *
 * a0 = p, a1 = end, a2 = out, a3 = w. Writes exactly w bytes, zero padding whatever
 * the stream does not cover. Knows nothing about zones or structs on purpose: the C
 * side works out the pointers, so this stays valid however the map format is wired up.
 *
 * Only x0..x15 exist in the embedded ABI, so this uses a0..a5, t0 and a3 and nothing
 * else. There is no multiply on this core and none is needed here.
 * ===================================================================================
 */
    .section .text
    .globl  map_row_rle
    .type   map_row_rle, @function
    .align  1
map_row_rle:
    add     a4, a2, a3          /* a4 = one past the last output byte */
.Ltop:
    bgeu    a2, a4, .Ldone      /* output full */
    bgeu    a0, a1, .Lpad       /* stream exhausted, zero the rest */
    lb      a5, 0(a0)           /* token, sign extended: bit seven becomes the sign */
    addi    a0, a0, 1
    bgez    a5, .Llit
    andi    a5, a5, 0x7f        /* run length */
    lbu     a3, 0(a0)           /* repeated value */
    addi    a0, a0, 1
.Lrun:
    beqz    a5, .Ltop
    bgeu    a2, a4, .Ldone
    sb      a3, 0(a2)
    addi    a2, a2, 1
    addi    a5, a5, -1
    j       .Lrun
.Llit:
    beqz    a5, .Ltop
    bgeu    a2, a4, .Ldone
    lbu     a3, 0(a0)
    addi    a0, a0, 1
    sb      a3, 0(a2)
    addi    a2, a2, 1
    addi    a5, a5, -1
    j       .Llit
.Lpad:
    li      a3, 0
.Lpad0:
    bgeu    a2, a4, .Ldone
    sb      a3, 0(a2)
    addi    a2, a2, 1
    j       .Lpad0
.Ldone:
    ret
    .size   map_row_rle, .-map_row_rle
