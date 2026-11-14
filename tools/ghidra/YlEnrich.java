// Post-analysis enrichment before YlExport:
//   1. Apply verified RTOS / HAL / libc / arch / jsmi / yglc / miio function names (never overwrites non-FUN_ names).
//   2. Label verified global RAM variables and WDT register.
//   3. Propagate recovered names onto thunk functions (thunk_<target>).
//
//@category Yeelight
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

import java.util.*;

public class YlEnrich extends GhidraScript {

    @Override
    protected void run() throws Exception {
        Map<Long, String> knownFuncs = new LinkedHashMap<>();
        // MTK7687/7697 HAL & FOTA
        knownFuncs.put(0x10071e98L, "hal_sys_reboot");
        knownFuncs.put(0x10072094L, "hal_wdt_init");
        knownFuncs.put(0x10072140L, "hal_wdt_software_reset");
        knownFuncs.put(0x10073278L, "hal_flash_init");
        knownFuncs.put(0x10073284L, "hal_flash_erase");
        knownFuncs.put(0x1007328eL, "hal_flash_read");
        knownFuncs.put(0x1007329cL, "hal_flash_write");
        knownFuncs.put(0x10086ca0L, "fota_trigger_update");

        // Tasks & startup
        knownFuncs.put(0x100a1ca8L, "n9log_task");
        knownFuncs.put(0x100a73d0L, "main_task");
        knownFuncs.put(0x100bab30L, "cli_task");
        knownFuncs.put(0x100baba8L, "cli_task_create");
        knownFuncs.put(0x100ecc7cL, "wifi_user_task");

        // arch_* & crash/dbg
        knownFuncs.put(0x100a7de4L, "arch_get_random");
        knownFuncs.put(0x100a7fa8L, "arch_crash_reboot");
        knownFuncs.put(0x100a8100L, "arch_dbg_log_begin");
        knownFuncs.put(0x100a8180L, "arch_dbg_log_end");
        knownFuncs.put(0x100a85d4L, "arch_os_ms_now");
        knownFuncs.put(0x100a8600L, "arch_os_ms_elapsed");
        knownFuncs.put(0x100a86f4L, "arch_os_thread_delete");
        knownFuncs.put(0x100a8704L, "arch_os_mbox_new");
        knownFuncs.put(0x100a8720L, "arch_os_mbox_free");
        knownFuncs.put(0x100a872cL, "arch_os_mbox_post");
        knownFuncs.put(0x100a877cL, "arch_os_mbox_fetch");
        knownFuncs.put(0x100a87d0L, "arch_os_sem_create");
        knownFuncs.put(0x100a87e8L, "arch_os_sem_get");
        knownFuncs.put(0x100a8828L, "arch_os_sem_put");
        knownFuncs.put(0x100a886cL, "arch_os_sem_delete");
        knownFuncs.put(0x100a8876L, "arch_os_mutex_create");
        knownFuncs.put(0x100a888eL, "arch_os_mutex_delete");
        knownFuncs.put(0x100a8898L, "arch_os_mutex_put");
        knownFuncs.put(0x100a88dcL, "arch_os_mutex_get");
        knownFuncs.put(0x100a891cL, "arch_os_queue_create");
        knownFuncs.put(0x100a8938L, "arch_os_queue_delete");
        knownFuncs.put(0x100a8944L, "arch_os_queue_send");
        knownFuncs.put(0x100a8984L, "arch_os_queue_recv");
        knownFuncs.put(0x100a89c4L, "arch_os_timer_create");
        knownFuncs.put(0x100a8a24L, "arch_os_timer_reset");
        knownFuncs.put(0x100a8a84L, "arch_os_timer_is_active");
        knownFuncs.put(0x100a8a98L, "arch_os_timer_activate");
        knownFuncs.put(0x100a8af8L, "arch_os_timer_deactivate");
        knownFuncs.put(0x100a8b9cL, "arch_os_timer_get_ctx");
        knownFuncs.put(0x100a8bc0L, "arch_os_async_call");

        // jsmi / JSON parser & composer
        knownFuncs.put(0x100a938cL, "jsmi_parse_finish");
        knownFuncs.put(0x100a93acL, "jsmi_get_tok");
        knownFuncs.put(0x100a9464L, "jsmi_get_value_sint");
        knownFuncs.put(0x100a9484L, "jsmi_get_value_uint");
        knownFuncs.put(0x100a94a4L, "jsmi_get_value_sint32");
        knownFuncs.put(0x100a94c2L, "jsmi_get_value_uint32");
        knownFuncs.put(0x100a94e0L, "jsmi_get_value_sint64");
        knownFuncs.put(0x100a9524L, "jsmi_get_value_sint16");
        knownFuncs.put(0x100a954aL, "jsmi_get_value_sint8");
        knownFuncs.put(0x100a9570L, "jsmi_get_value_str");
        knownFuncs.put(0x100a9658L, "jsmi_get_value_bytes");
        knownFuncs.put(0x100a968cL, "jsmi_get_value_bool");
        knownFuncs.put(0x100a97d4L, "jsmi_compose_start");
        knownFuncs.put(0x100a97f0L, "jsmi_compose_finish");
        knownFuncs.put(0x100a9810L, "jsmi_set_object_begin");
        knownFuncs.put(0x100a9848L, "jsmi_set_object_end");
        knownFuncs.put(0x100a9910L, "jsmi_set_key");
        knownFuncs.put(0x100a99c8L, "jsmi_set_value_sint");
        knownFuncs.put(0x100a9a00L, "jsmi_set_value_uint");
        knownFuncs.put(0x100a9b50L, "jsmi_set_key_value_sint");
        knownFuncs.put(0x100a9b90L, "jsmi_set_key_value_uint");
        knownFuncs.put(0x100a9c18L, "jsmi_set_key_value_str");
        knownFuncs.put(0x100a9c98L, "jsmi_set_key_value_double");
        knownFuncs.put(0x100a9d0cL, "jsmi_set_key_object_begin");
        knownFuncs.put(0x100a9d44L, "jsmi_set_key_object_end");
        knownFuncs.put(0x100a9d8cL, "jsmi_set_key_array_begin");
        knownFuncs.put(0x100a9dc4L, "jsmi_set_key_array_end");

        // yglc / yl_cache timers & warm reboot
        knownFuncs.put(0x100bebf0L, "yglc_check_fast_reboot");
        knownFuncs.put(0x100becd8L, "yglc_fast_reboot_count_zero");
        knownFuncs.put(0x100bed64L, "yglc_reboot_save");
        knownFuncs.put(0x100bed94L, "yglc_reboot_restore");
        knownFuncs.put(0x100c34fcL, "yglc_timer_clock_init");
        knownFuncs.put(0x100c354cL, "yglc_timer_add");
        knownFuncs.put(0x100c358eL, "yglc_timer_is_running");
        knownFuncs.put(0x100c35a0L, "yglc_timer_del");

        // FreeRTOS
        knownFuncs.put(0x100c729cL, "prvIdleTask");
        knownFuncs.put(0x100c738cL, "xTaskCreate");
        knownFuncs.put(0x100c73e8L, "vTaskDelete");
        knownFuncs.put(0x100c74c0L, "vTaskDelay");
        knownFuncs.put(0x100c7624L, "vTaskSuspendAll");
        knownFuncs.put(0x100c7634L, "xTaskGetTickCount");
        knownFuncs.put(0x100c7640L, "xTaskGetTickCountFromISR");
        knownFuncs.put(0x100c7794L, "xTaskResumeAll");
        knownFuncs.put(0x100c80ccL, "xQueueGenericCreate");
        knownFuncs.put(0x100c8168L, "xQueueGenericSend");
        knownFuncs.put(0x100c8304L, "xQueueCreateMutex");
        knownFuncs.put(0x100c835cL, "xQueueGenericSendFromISR");
        knownFuncs.put(0x100c8430L, "xQueueGiveFromISR");
        knownFuncs.put(0x100c84e0L, "xQueueGenericReceive");
        knownFuncs.put(0x100c8620L, "xQueueSemaphoreTake");
        knownFuncs.put(0x100c87e0L, "xQueueReceiveFromISR");
        knownFuncs.put(0x100c8920L, "vQueueDelete");
        knownFuncs.put(0x100c8a94L, "xTimerCreate");
        knownFuncs.put(0x100c8ae4L, "xTimerGenericCommand");
        knownFuncs.put(0x100c8becL, "prvTimerTask");
        knownFuncs.put(0x100c8dacL, "xTimerIsTimerActive");
        knownFuncs.put(0x100c8ddcL, "pvTimerGetTimerID");
        knownFuncs.put(0x100c8e04L, "prvInsertBlockIntoFreeList");
        knownFuncs.put(0x100c8ea8L, "pvPortMalloc");
        knownFuncs.put(0x100c8fe4L, "vPortFree");
        knownFuncs.put(0x100c9068L, "xPortGetFreeHeapSize");
        knownFuncs.put(0x100c9074L, "pvPortCalloc");
        knownFuncs.put(0x100c9090L, "pvPortRealloc");
        knownFuncs.put(0x100c91fcL, "vPortEnterCritical");
        knownFuncs.put(0x100c9240L, "vPortExitCritical");
        knownFuncs.put(0x100c938cL, "vPortValidateInterruptPriority");
        knownFuncs.put(0x100c9d5cL, "vApplicationAssert");

        // miio & psm
        knownFuncs.put(0x100f7ed0L, "miio_reboot_timer_cb");
        knownFuncs.put(0x100f819cL, "miio_set_event");
        knownFuncs.put(0x100f81e4L, "miio_reboot");
        knownFuncs.put(0x10104930L, "psm_get_variable");
        knownFuncs.put(0x10104974L, "psm_set_variable");
        knownFuncs.put(0x101049c8L, "psm_erase_variable");
        knownFuncs.put(0x10104a0cL, "nvdm_psm_init");
        knownFuncs.put(0x10104aa0L, "arch_psm_get_value");
        knownFuncs.put(0x10104b14L, "arch_psm_set_value");
        knownFuncs.put(0x10104b8cL, "arch_psm_get_str");
        knownFuncs.put(0x10104c08L, "arch_psm_set_str");
        knownFuncs.put(0x10104ca4L, "arch_psm_get_country_domain");

        // newlib / libc
        knownFuncs.put(0x10106d5eL, "atoi");
        knownFuncs.put(0x10106d66L, "bzero");
        knownFuncs.put(0x10107534L, "memchr");
        knownFuncs.put(0x10107552L, "memcmp");
        knownFuncs.put(0x10107570L, "memcpy");
        knownFuncs.put(0x10107582L, "memmove");
        knownFuncs.put(0x101075b8L, "memset");
        knownFuncs.put(0x10107f90L, "snprintf");
        knownFuncs.put(0x10107ff4L, "sprintf");
        knownFuncs.put(0x1010803cL, "sscanf");
        knownFuncs.put(0x1010811aL, "strcat");
        knownFuncs.put(0x10108134L, "strchr");
        knownFuncs.put(0x1010814eL, "strcmp");
        knownFuncs.put(0x10108162L, "strcpy");
        knownFuncs.put(0x1010854eL, "strlen");
        knownFuncs.put(0x101085a4L, "strncmp");
        knownFuncs.put(0x101085d2L, "strncpy");
        knownFuncs.put(0x101085f6L, "strnlen");
        knownFuncs.put(0x10108660L, "strstr");
        knownFuncs.put(0x101094acL, "strtol");

        Set<String> used = new HashSet<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            used.add(f.getName());
        }

        int renamed = 0;
        for (Map.Entry<Long, String> e : knownFuncs.entrySet()) {
            Function f = getFunctionAt(toAddr(e.getKey()));
            if (f == null || !f.getName().startsWith("FUN_")) continue;
            String base = e.getValue(), cand = base;
            for (int i = 2; used.contains(cand); i++) cand = base + "_" + i;
            f.setName(cand, SourceType.USER_DEFINED);
            used.add(cand);
            renamed++;
        }

        // Propagate thunk names immediately as well (YlExport will propagate log-named thunks later)
        int thunks = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!f.isThunk()) continue;
            if (!f.getName().startsWith("thunk_FUN_") && !f.getName().startsWith("FUN_")) continue;
            Function tgt = f.getThunkedFunction(true);
            if (tgt == null || tgt.getName().startsWith("FUN_")) continue;
            String base = "thunk_" + tgt.getName(), cand = base;
            for (int i = 2; used.contains(cand); i++) cand = base + "_" + i;
            f.setName(cand, SourceType.USER_DEFINED);
            used.add(cand);
            thunks++;
        }

        // Label verified global variables / MMIO registers
        Map<Long, String> globals = new LinkedHashMap<>();
        globals.put(0x20004b24L, "g_miio_net");
        globals.put(0x20006908L, "g_yglc_settings_ptr");
        globals.put(0x20008030L, "g_offline_since_ms");
        globals.put(0x2000805cL, "g_yl_cache_list");
        globals.put(0x2000806cL, "g_yl_cache_schema_crc");
        globals.put(0x20008074L, "g_warm_boot_marker");
        globals.put(0x20008084L, "g_warm_boot_type");
        globals.put(0x83080030L, "WDT_SWRST");

        SymbolTable st = currentProgram.getSymbolTable();
        int labeled = 0;
        for (Map.Entry<Long, String> e : globals.entrySet()) {
            Address a = toAddr(e.getKey());
            if (currentProgram.getMemory().getBlock(a) == null) continue;
            Symbol primary = st.getPrimarySymbol(a);
            if (primary == null || primary.getName().startsWith("DAT_") || primary.isDynamic()) {
                st.createLabel(a, e.getValue(), SourceType.USER_DEFINED);
                labeled++;
            }
        }

        println("YlEnrich: renamed " + renamed + " functions, " + thunks + " thunks, labeled " + labeled + " globals");
    }
}
