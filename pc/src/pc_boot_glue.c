/* Boot-time glue whose hardware definition is mwcc asm in the SDK's crt0.
 *
 * _start_AutoloadDoneCallback is declared by crt0 as a weak, empty
 * user-overridable hook, the startup code branches to it once autoload
 * sections are in place, and the SDK's own default does nothing. An empty
 * function is that default, not a stub: there is no autoload step to
 * observe here in the first place (every section is statically linked).
 */
void _start_AutoloadDoneCallback(void *argv[])
{
    (void)argv;
}
