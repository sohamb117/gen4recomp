/*
 * What the shared host layer reaches in Diamond's decompiled half
 * (games/diamond/pc/game) that a ROM-only core does not have.
 *
 * games/diamond/pc/src/pc_dp_hooks.c answers Platinum's lab hooks by
 * calling D's save lab and heap checker, both of which walk the
 * decompiled game's own structures by their C names. A recompiled ROM has
 * no such names, so here they are inert: the heap check level stays 0 (the
 * hook never calls pc_dp_heap_check) and the lab sees nothing to do.
 */
int pc_dp_heapcheck_level;
unsigned long long pc_dp_heapcheck_frame;

void pc_dp_heap_check(const char *where)
{
    (void)where;
}

void pc_dp_lab_frame(unsigned long long frame)
{
    (void)frame;
}
