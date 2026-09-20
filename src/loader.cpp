/*
 * mod-bot-economy loader.
 *
 * The playerbots fork auto-globs every module's sources into one lib and looks
 * up a loader symbol derived from the folder name: for folder "mod-bot-economy"
 * that symbol is exactly "Addmod_bot_economyScripts". It must exist and call
 * our real registration function.
 */

void AddBotEconomyScripts();

void Addmod_bot_economyScripts()
{
    AddBotEconomyScripts();
}
