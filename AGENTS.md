# CHIM Agent Notes

- The in-game Settings hub must stay aligned with the HerikaServer Global Settings, Profiles, and CHIM NPC APIs.
- When HerikaServer adds, removes, renames, or changes a Global Settings/Profile field, update and validate `AIAgent/PrismaUI/views/CHIM/config_manager.*` in the paired change.
- Keep CHIM NPC editing routed through the existing `npc_manager.*` view and `chim_npc_manager.php` API instead of creating a second editor.
- Validate keyboard capture is enabled only while an editable control owns focus and is released when the Settings hub closes or switches tabs.
