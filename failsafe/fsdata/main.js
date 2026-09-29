/* Compact MT7621 recovery UI inspired by bl-mt798x-dhcpd. */
(function () {
    "use strict";

    var STORE_LANG = "failsafe_lang";
    var STORE_THEME = "failsafe_theme";
    var state = window.APP_STATE = window.APP_STATE || {
        page: "",
        sysinfo: null,
        sysinfoError: false,
        backupStatusKey: "",
        backupinfo: null,
        overclock: null,
        overclockStatusKey: "",
        params: null,
        paramsStatusKey: ""
    };

    var text = {
        en: {
            brand: "Recovery Mode WEBUI",
            nav_recovery: "Recovery",
            nav_tools: "Tools",
            nav_advanced: "Advanced",
            nav_system: "System",
            nav_firmware: "Firmware upgrade",
            nav_official: "Xiaomi firmware restore",
            nav_uboot: "Update U-Boot",
            nav_initramfs: "Load Initramfs",
            nav_factory: "Update Factory",
            nav_backup: "Flash backup",
            nav_overclock: "CPU overclock",
            nav_params: "Parameter editor",
            nav_reboot: "Reboot device",
            ctl_language: "Language",
            ctl_theme: "Theme",
            opt_auto: "Auto",
            opt_light: "Light",
            opt_dark: "Dark",
            opt_zh: "简体中文",
            opt_ru: "Русский",
            opt_en: "English",
            page_firmware_title: "Firmware upgrade",
            h_firmware: "Firmware upgrade",
            hint_firmware: "Install a standard <strong>OpenWrt / ImmortalWrt / LEDE sysupgrade</strong> or <strong>Padavan firmware</strong>.<br>Select the correct file for this device, then upload it.",
            page_official_title: "Restore Xiaomi firmware",
            h_official: "Restore Xiaomi firmware",
            hint_official: "Restore an official <strong>Xiaomi HDR1 firmware</strong> image.<br>The stock dual kernel/rootfs layout will be rebuilt automatically.",
            page_uboot_title: "U-Boot update",
            h_uboot: "Update U-Boot",
            hint_uboot: "Replace the device <strong>bootloader</strong>.<br>Only use a U-Boot image built for this exact device.",
            page_initramfs_title: "Load Initramfs",
            h_initramfs: "Load Initramfs",
            hint_initramfs: "Upload an <strong>initramfs recovery image</strong> and boot it directly from memory.",
            page_factory_title: "Factory update",
            h_factory: "Update Factory",
            hint_factory: "Replace the <strong>wireless calibration data</strong> stored in the Factory partition.",
            page_backup_title: "Flash backup",
            h_backup: "Flash backup",
            hint_backup: "Download a raw partition, flash device, or selected byte range to your computer.",
            page_overclock_title: "CPU overclock",
            h_overclock: "CPU overclock",
            hint_overclock: "Set the <strong>MT7621 CPU frequency</strong> used for normal system boot.<br>U-Boot and Web recovery always start at the safe frequency.",
            oc_current: "Current recovery frequency",
            oc_safe: "Safe frequency",
            oc_saved: "Saved setting",
            oc_target: "CPU frequency (MHz)",
            oc_disabled: "Disabled",
            oc_range: "Allowed range: {min}–{max} MHz, in {step} MHz steps.",
            oc_save: "Save frequency",
            oc_disable: "Use safe frequency",
            oc_confirm: "Save this CPU frequency? An unsuitable value may cause frequent system hangs.",
            oc_disable_confirm: "Disable CPU overclock and restore the safe frequency on the next boot?",
            oc_status_loading: "Reading CPU frequency setting…",
            oc_status_saved: "Saved. The new frequency is applied only when the operating system starts next time.",
            oc_status_disabled: "Overclock disabled. The safe frequency will be used on the next boot.",
            oc_status_error: "Unable to save this frequency. Use a value within the displayed range and step.",
            oc_status_unavailable: "CPU frequency settings are unavailable.",
            oc_warn_hang: "An unsuitable frequency may cause frequent system hangs.",
            oc_warn_data: "Instability can corrupt data; increase the frequency gradually.",
            oc_warn_recovery: "If the system cannot boot, power off, hold Reset for 1–10 seconds while powering on, then clear this setting in Web recovery.",
            page_params_title: "Parameter editor",
            h_params: "Parameter editor",
            hint_params: "Edit Xiaomi <strong>stock parameters</strong> and interface <strong>MAC addresses</strong>.<br>The current records are checked before any write.",
            params_backup: "Back up Config, Bdata and Factory first",
            params_reload: "Reload",
            params_config_title: "Stock environment (Config)",
            params_config_hint: "4 KiB CRC32 record at 0x80000. These values control stock boot and first-boot settings.",
            params_bdata_title: "Factory identity (Bdata)",
            params_bdata_hint: "16 KiB CRC32 record at 0xC0000. It normally contains the serial number, country, model, color and stock SSIDs.",
            params_mac_title: "MAC addresses (Factory)",
            params_mac_hint: "Only the four verified MAC fields are changed. They must share one prefix and remain consecutive in WAN → LAN → RF1 → RF2 order; all surrounding Wi-Fi calibration data is preserved byte-for-byte.",
            params_field: "Field",
            params_value: "Value",
            params_add: "Add field",
            params_delete: "Delete",
            params_save_config: "Save Config",
            params_save_bdata: "Save Bdata",
            params_save_macs: "Save MAC addresses",
            params_mac_rf1: "RF1 WLAN MAC",
            params_mac_rf2: "RF2 WLAN MAC",
            params_mac_lan: "LAN MAC",
            params_mac_wan: "WAN MAC",
            params_crc_ok: "CRC verified",
            params_crc_bad: "CRC invalid — writing disabled",
            params_read_ok: "Read successfully",
            params_read_bad: "Unable to read",
            params_layout_bad: "Layout mismatch — writing disabled",
            params_loading: "Reading Config, Bdata and Factory…",
            params_saved: "Written and read-back verified successfully.",
            params_error: "Unable to save. Check the values and serial console error.",
            params_invalid: "Fields must have unique non-empty names and valid values.",
            params_mac_invalid: "Use four consecutive unicast addresses with one prefix, ordered WAN → LAN → RF1 → RF2.",
            params_confirm_env: "Write this complete parameter table? Incorrect boot values may stop Xiaomi stock firmware from starting.",
            params_confirm_macs: "Write these MAC addresses to Factory? Incorrect values can break networking.",
            params_warn_backup: "Back up Config, Bdata and Factory before making changes.",
            params_warn_power: "Do not disconnect power while an erase block is being written and verified.",
            params_warn_stock: "Deleting or changing boot fields may prevent Xiaomi stock firmware from starting.",
            params_warn_factory: "Incorrect MAC addresses can break networking or permanently duplicate another device identity.",
            page_reboot_title: "Reboot device",
            h_reboot: "Rebooting device",
            reboot_info: "The reboot request has been sent. Please wait for the device to start again.",
            reboot_warn_1: "do not remove power while the device is restarting",
            reboot_confirm: "Reboot the device now?",
            page_flashing_title: "Update in progress",
            h_update_in_progress: "Update in progress",
            p_update_in_progress: "The file was uploaded successfully. Flashing may take several minutes.<br>Do not close this page or disconnect power.",
            h_update_completed: "Update completed",
            p_update_completed: "The image was written and verified. The device is restarting automatically…",
            page_booting_title: "Booting Initramfs",
            h_booting_initramfs: "Booting Initramfs",
            p_booting_initramfs: "The image was uploaded. Please wait while the device starts from memory.",
            h_boot_success: "Initramfs started",
            p_boot_success: "The device accepted the image and is switching to Initramfs.",
            page_fail_title: "Update failed",
            h_update_failed: "Update failed",
            fail_detail_html: "<strong>The operation did not complete.</strong>Check that the image and device match, then retry. The serial console contains the detailed error.",
            page_success_title: "Success",
            h_success: "Success",
            p_success: "The operation completed successfully.",
            page_404_title: "404 - Not found",
            page_not_found: "The requested recovery page does not exist.",
            other_warnings: "Important",
            warn_no_poweroff: "do not disconnect power while flash is being erased or written",
            warn_restart: "after a successful firmware update, the device restarts automatically",
            warn_choose_fw: "BOARD matching is disabled; you must select the correct device firmware yourself",
            warn_official_model: "only an exact R2100/RM2100 official image matching this device is accepted",
            warn_official_layout: "the installed OpenWrt system and overlay will be erased; calibration and device identity are preserved",
            warn_choose_uboot: "a bootloader for another model can make the device unbootable",
            warn_uboot_danger: "keep a full-flash backup and serial/programmer recovery available",
            warn_boot_initramfs: "Initramfs runs from memory and does not replace the installed system",
            warn_choose_initramfs: "use an MT7621 image made for this device",
            warn_factory_danger: "incorrect calibration data can break Wi-Fi or permanently change device identity",
            btn_upload: "Upload",
            btn_update: "Install now",
            btn_restore_official: "Restore now",
            btn_boot: "Boot now",
            prompt_update: "Upload complete. Verify the file information before installing.",
            prompt_official: "Upload complete. Verify the model and file information before restoring.",
            prompt_boot: "Upload complete. Verify the file information before booting.",
            file_label: "File:",
            size_label: "Size:",
            upload_progress: "Uploading",
            upload_none: "Choose a file first.",
            upload_error: "Upload failed. Check the cable and try again.",
            sysinfo_loading: "Loading device information…",
            sysinfo_unavailable: "Device information is unavailable.",
            sysinfo_board: "Device:",
            sysinfo_ram: "Memory:",
            sysinfo_flash: "Flash:",
            sysinfo_flash_nmbm: "NMBM:",
            sysinfo_compat: "Compatible:",
            sysinfo_mtdparts: "Partitions:",
            sysinfo_mtdids: "Mapping:",
            sysinfo_more: "Technical details",
            sysinfo_unknown: "Unknown",
            backup_label_mode: "Mode:",
            backup_label_target: "Target:",
            backup_label_start: "Start:",
            backup_label_end: "End (exclusive):",
            backup_mode_part: "Partition / full flash",
            backup_mode_range: "Custom range",
            backup_btn_download: "Download backup",
            backup_target_placeholder: "-- select target --",
            backup_target_full_disk: "Full flash",
            backup_range_hint: "Accepted formats: decimal, 0xHEX, or KiB (for example 64KiB).",
            backup_warn_1: "do not disconnect power during a backup",
            backup_warn_2: "custom ranges read raw bytes; verify both offsets",
            backup_warn_3: "large backups can take several minutes",
            backup_status_starting: "Preparing backup…",
            backup_status_downloading: "Downloading:",
            backup_status_preparing: "Preparing file…",
            backup_status_done: "Saved:",
            backup_error_no_target: "Select a backup target.",
            backup_error_bad_range: "Enter a valid start and end.",
            backup_error_http: "Backup request failed:",
            backup_error_exception: "Backup failed:"
        },
        zh: {
            brand: "恢复模式 WEBUI",
            nav_recovery: "恢复功能",
            nav_tools: "维护工具",
            nav_advanced: "高级功能",
            nav_system: "系统",
            nav_firmware: "固件升级",
            nav_official: "恢复小米官方固件",
            nav_uboot: "更新 U-Boot",
            nav_initramfs: "加载 Initramfs",
            nav_factory: "更新 Factory",
            nav_backup: "闪存备份",
            nav_overclock: "CPU 超频",
            nav_params: "参数修改",
            nav_reboot: "重启设备",
            ctl_language: "语言",
            ctl_theme: "主题",
            opt_auto: "自动",
            opt_light: "亮色",
            opt_dark: "暗色",
            opt_zh: "简体中文",
            opt_ru: "Русский",
            opt_en: "English",
            page_firmware_title: "固件升级",
            h_firmware: "固件升级",
            hint_firmware: "安装标准的 <strong>OpenWrt / ImmortalWrt / LEDE sysupgrade</strong> 和 <strong>Padavan 固件</strong>。<br>请选择适配本设备的文件，然后上传。",
            page_official_title: "恢复小米官方固件",
            h_official: "恢复小米官方固件",
            hint_official: "恢复小米官方 <strong>HDR1 固件</strong>。<br>程序会自动重建原厂双 kernel/rootfs 分区布局。",
            page_uboot_title: "更新 U-Boot",
            h_uboot: "更新 U-Boot",
            hint_uboot: "替换设备的<strong>引导程序</strong>。<br>只能使用为当前设备编译的 U-Boot 镜像。",
            page_initramfs_title: "加载 Initramfs",
            h_initramfs: "加载 Initramfs",
            hint_initramfs: "上传 <strong>initramfs 恢复镜像</strong>，直接从内存启动。",
            page_factory_title: "更新 Factory",
            h_factory: "更新 Factory",
            hint_factory: "替换 Factory 分区中的<strong>无线校准数据</strong>。",
            page_backup_title: "闪存备份",
            h_backup: "闪存备份",
            hint_backup: "将原始分区、整颗闪存或指定字节范围下载到电脑。",
            page_overclock_title: "CPU 超频",
            h_overclock: "CPU 超频",
            hint_overclock: "手动设置正常启动系统时使用的 <strong>MT7621 CPU 频率</strong>。<br>U-Boot 和 Web 恢复模式始终以安全频率启动。",
            oc_current: "当前恢复模式频率",
            oc_safe: "安全频率",
            oc_saved: "已保存设置",
            oc_target: "CPU 频率（MHz）",
            oc_disabled: "未启用",
            oc_range: "允许范围：{min}–{max} MHz，步进 {step} MHz。",
            oc_save: "保存频率",
            oc_disable: "恢复安全频率",
            oc_confirm: "确定保存这个 CPU 频率吗？不合适的频率可能导致频繁死机。",
            oc_disable_confirm: "确定关闭 CPU 超频，并在下次启动时恢复安全频率吗？",
            oc_status_loading: "正在读取 CPU 频率设置…",
            oc_status_saved: "设置已保存；新频率只会在下次正常启动系统时应用。",
            oc_status_disabled: "超频已关闭；下次启动将使用安全频率。",
            oc_status_error: "无法保存该频率，请使用页面显示范围和步进内的数值。",
            oc_status_unavailable: "CPU 频率设置不可用。",
            oc_warn_hang: "不合适的频率可能导致频繁死机。",
            oc_warn_data: "不稳定可能损坏数据，请逐步提高频率并充分测试。",
            oc_warn_recovery: "如果系统无法启动，请断电，通电时按住 Reset 1–10 秒进入 Web 恢复，然后在本页清除设置。",
            page_params_title: "参数修改",
            h_params: "参数修改",
            hint_params: "修改小米<strong>原厂参数</strong>和各接口的 <strong>MAC 地址</strong>。<br>每次写入前都会检查现有记录，写入后会完整回读校验。",
            params_backup: "请先备份 Config、Bdata 和 Factory",
            params_reload: "重新读取",
            params_config_title: "原厂环境变量（Config）",
            params_config_hint: "整机偏移 0x80000 的 4 KiB CRC32 记录，包含原厂启动和首次配置参数。",
            params_bdata_title: "原厂身份参数（Bdata）",
            params_bdata_hint: "整机偏移 0xC0000 的 16 KiB CRC32 记录，通常包含序列号、地区、型号、颜色和原厂 SSID。",
            params_mac_title: "MAC 地址（Factory）",
            params_mac_hint: "只修改已经验证的四个 MAC 字段；四者必须保持相同前缀，并按 WAN → LAN → RF1 → RF2 连续递增。周围的无线校准数据会逐字节保留。",
            params_field: "字段",
            params_value: "值",
            params_add: "添加字段",
            params_delete: "删除",
            params_save_config: "保存 Config",
            params_save_bdata: "保存 Bdata",
            params_save_macs: "保存 MAC 地址",
            params_mac_rf1: "RF1 WLAN MAC",
            params_mac_rf2: "RF2 WLAN MAC",
            params_mac_lan: "LAN MAC",
            params_mac_wan: "WAN MAC",
            params_crc_ok: "CRC 校验通过",
            params_crc_bad: "CRC 异常，已禁止写入",
            params_read_ok: "读取正常",
            params_read_bad: "读取失败",
            params_layout_bad: "布局不匹配，已禁止写入",
            params_loading: "正在读取 Config、Bdata 和 Factory…",
            params_saved: "写入完成，回读校验通过。",
            params_error: "保存失败，请检查输入内容和串口错误信息。",
            params_invalid: "字段名不能为空或重复，内容也不能包含换行。",
            params_mac_invalid: "请输入同一前缀的四个连续单播 MAC，顺序必须为 WAN → LAN → RF1 → RF2。",
            params_confirm_env: "确定写入完整参数表吗？错误的启动参数可能导致小米原厂系统无法启动。",
            params_confirm_macs: "确定把这些 MAC 地址写入 Factory 吗？错误地址可能导致网络异常。",
            params_warn_backup: "修改前必须备份 Config、Bdata 和 Factory 分区。",
            params_warn_power: "擦除块写入和回读校验期间严禁断电。",
            params_warn_stock: "删除或修改启动字段可能导致小米原厂固件无法启动。",
            params_warn_factory: "错误 MAC 会导致网络异常，或与其他设备产生永久地址冲突。",
            page_reboot_title: "重启设备",
            h_reboot: "设备正在重启",
            reboot_info: "重启请求已经发送，请等待设备重新启动。",
            reboot_warn_1: "设备重启期间请勿断电",
            reboot_confirm: "确定立即重启设备？",
            page_flashing_title: "正在升级",
            h_update_in_progress: "正在升级",
            p_update_in_progress: "文件已上传，刷写可能需要几分钟。<br>请勿关闭页面、拔网线或断开电源。",
            h_update_completed: "升级完成",
            p_update_completed: "镜像已经写入并校验，设备正在自动重启…",
            page_booting_title: "正在启动 Initramfs",
            h_booting_initramfs: "正在启动 Initramfs",
            p_booting_initramfs: "镜像已上传，请等待设备从内存启动。",
            h_boot_success: "Initramfs 已启动",
            p_boot_success: "设备已接受镜像，正在切换到 Initramfs。",
            page_fail_title: "升级失败",
            h_update_failed: "升级失败",
            fail_detail_html: "<strong>操作未能完成。</strong>请确认镜像与设备匹配后重试；串口控制台中会显示详细错误。",
            page_success_title: "操作成功",
            h_success: "操作成功",
            p_success: "操作已经完成。",
            page_404_title: "404 - 页面不存在",
            page_not_found: "请求的恢复页面不存在。",
            other_warnings: "重要提示",
            warn_no_poweroff: "擦除或写入闪存期间严禁断电",
            warn_restart: "固件升级成功后设备会自动重启",
            warn_choose_fw: "已关闭 BOARD 机型匹配，请自行确认固件型号",
            warn_official_model: "仅接受与本机完全匹配的 R2100／RM2100 官方固件，设备 ID 不符会拒绝刷写",
            warn_official_layout: "当前 OpenWrt 系统和 overlay 会被清除，但 Bootloader、校准数据和设备身份会保留",
            warn_choose_uboot: "刷入其他机型的引导程序可能导致设备无法启动",
            warn_uboot_danger: "请提前准备完整闪存备份、串口和编程器恢复手段",
            warn_boot_initramfs: "Initramfs 从内存运行，不会替换已安装系统",
            warn_choose_initramfs: "请使用为当前设备制作的 MT7621 镜像",
            warn_factory_danger: "错误校准数据会导致 Wi-Fi 异常或永久改变设备身份",
            btn_upload: "上传",
            btn_update: "立即安装",
            btn_restore_official: "立即恢复",
            btn_boot: "立即启动",
            prompt_update: "上传完成，请核对文件信息后再安装。",
            prompt_official: "上传完成，请核对机型和文件信息后再恢复。",
            prompt_boot: "上传完成，请核对文件信息后再启动。",
            file_label: "文件：",
            size_label: "大小：",
            upload_progress: "正在上传",
            upload_none: "请先选择文件。",
            upload_error: "上传失败，请检查网线后重试。",
            sysinfo_loading: "正在读取设备信息…",
            sysinfo_unavailable: "无法读取设备信息。",
            sysinfo_board: "设备：",
            sysinfo_ram: "内存：",
            sysinfo_flash: "闪存：",
            sysinfo_flash_nmbm: "NMBM：",
            sysinfo_compat: "兼容标识：",
            sysinfo_mtdparts: "分区表：",
            sysinfo_mtdids: "设备映射：",
            sysinfo_more: "技术详情",
            sysinfo_unknown: "未知",
            backup_label_mode: "模式：",
            backup_label_target: "目标：",
            backup_label_start: "起始：",
            backup_label_end: "结束（不包含）：",
            backup_mode_part: "分区／整颗闪存",
            backup_mode_range: "自定义范围",
            backup_btn_download: "下载备份",
            backup_target_placeholder: "-- 请选择目标 --",
            backup_target_full_disk: "整颗闪存",
            backup_range_hint: "支持十进制、0x 十六进制或 KiB，例如 64KiB。",
            backup_warn_1: "备份过程中请勿断电",
            backup_warn_2: "自定义范围读取原始字节，请核对起止偏移",
            backup_warn_3: "大容量备份可能需要几分钟",
            backup_status_starting: "正在准备备份…",
            backup_status_downloading: "正在下载：",
            backup_status_preparing: "正在生成文件…",
            backup_status_done: "已保存：",
            backup_error_no_target: "请选择备份目标。",
            backup_error_bad_range: "请输入有效的起始和结束位置。",
            backup_error_http: "备份请求失败：",
            backup_error_exception: "备份失败："
        }
,
        ru: {
            brand: "Режим восстановления системы",
            nav_recovery: "Восстановление",
            nav_tools: "Инструменты",
            nav_advanced: "Дополнительно",
            nav_system: "Система",
            nav_firmware: "Обновление прошивки",
            nav_official: "Прошивка Xiaomi",
            nav_uboot: "Обновление U-Boot",
            nav_initramfs: "Загрузка Initramfs",
            nav_factory: "Обновление Factory",
            nav_backup: "Резервная копия",
            nav_overclock: "Разгон CPU",
            nav_params: "Параметры",
            nav_reboot: "Перезагрузка",
            ctl_language: "🌐 Язык",
            ctl_theme: "🎨 Тема",
            opt_auto: "Авто",
            opt_light: "Светлая",
            opt_dark: "Тёмная",
            opt_zh: "中文",
            opt_ru: "Русский",
            opt_en: "English",
            page_firmware_title: "Обновление прошивки",
            h_firmware: "ОБНОВЛЕНИЕ ПРОШИВКИ",
            hint_firmware: "Вы собираетесь обновить <strong>прошивку<\\/strong> устройства.<br>Выберите файл на локальном диске и нажмите кнопку <strong>Загрузить<\\/strong>.",
            page_official_title: "Восстановление прошивки Xiaomi",
            h_official: "Восстановление прошивки Xiaomi",
            hint_official: "Восстановление официального образа <strong>Xiaomi HDR1</strong>.<br>Стандартная двойная разметка ядра и корневой ФС будет восстановлена автоматически.",
            page_uboot_title: "Обновление U-Boot",
            h_uboot: "ОБНОВЛЕНИЕ U-BOOT",
            hint_uboot: "Вы собираетесь обновить <strong>U-Boot (загрузчик)<\\/strong> устройства.<br>Выберите файл на локальном диске и нажмите кнопку <strong>Загрузить<\\/strong>.",
            page_initramfs_title: "Загрузка initramfs",
            h_initramfs: "ЗАГРУЗКА INITRAMFS",
            hint_initramfs: "Вы собираетесь загрузить <strong>initramfs<\\/strong> на устройстве.<br>Выберите файл на локальном диске и нажмите кнопку <strong>Загрузить<\\/strong>.",
            page_factory_title: "Обновление Factory",
            h_factory: "ОБНОВЛЕНИЕ FACTORY",
            hint_factory: "Вы собираетесь обновить раздел <strong>Factory (калибровка беспроводного модуля)<\\/strong> на устройстве.<br>Выберите файл на локальном диске и нажмите кнопку <strong>Загрузить<\\/strong>.",
            page_backup_title: "Резервная копия",
            h_backup: "РЕЗЕРВНАЯ КОПИЯ",
            hint_backup: "Скачайте резервную копию из хранилища устройства как <strong>двоичный файл<\\/strong>.<br>Данные будут переданы в браузер и сохранены на вашем компьютере.",
            page_overclock_title: "Разгон CPU",
            h_overclock: "Разгон CPU",
            hint_overclock: "Задайте <strong>частоту CPU MT7621</strong> для обычной загрузки системы.<br>U-Boot и веб-восстановление всегда запускаются на безопасной частоте.",
            oc_current: "Текущая частота восстановления",
            oc_safe: "Безопасная частота",
            oc_saved: "Сохранённое значение",
            oc_target: "Частота CPU (МГц)",
            oc_disabled: "Отключено",
            oc_range: "Допустимый диапазон: {min}–{max} МГц с шагом {step} МГц.",
            oc_save: "Сохранить частоту",
            oc_disable: "Использовать безопасную частоту",
            oc_confirm: "Сохранить эту частоту CPU? Неподходящее значение может приводить к частым зависаниям системы.",
            oc_disable_confirm: "Отключить разгон CPU и вернуть безопасную частоту при следующей загрузке?",
            oc_status_loading: "Чтение настройки частоты CPU…",
            oc_status_saved: "Сохранено. Новая частота применится только после запуска операционной системы.",
            oc_status_disabled: "Разгон отключён. При следующей загрузке будет использована безопасная частота.",
            oc_status_error: "Не удалось сохранить эту частоту. Используйте значение в указанном диапазоне.",
            oc_status_unavailable: "Настройки частоты CPU недоступны.",
            oc_warn_hang: "Неудачная частота может приводить к частым зависаниям системы.",
            oc_warn_data: "Нестабильность может повредить данные; увеличивайте частоту постепенно.",
            oc_warn_recovery: "Если система не загружается, отключите питание, удерживайте Reset 1–10 секунд при включении, затем включите питание и откройте <strong>http://192.168.1.1</strong>.",
            page_params_title: "Редактор параметров",
            h_params: "Редактор параметров",
            hint_params: "Редактирование заводских <strong>параметров Xiaomi</strong> и адресов <strong>MAC</strong> интерфейсов.<br>Текущие записи проверяются перед любой записью.",
            params_backup: "Сначала сохраните Config, Bdata и Factory",
            params_reload: "Перечитать",
            params_config_title: "Заводское окружение (Config)",
            params_config_hint: "Запись 4 КиБ с CRC32 по адресу 0x80000. Эти значения управляют загрузкой стоковой прошивки и настройками первого включения.",
            params_bdata_title: "Заводская идентификация (Bdata)",
            params_bdata_hint: "Запись 16 КиБ с CRC32 по адресу 0xC0000. Обычно содержит серийный номер, страну, модель, цвет и заводские имена сетей Wi-Fi.",
            params_mac_title: "Адреса MAC (Factory)",
            params_mac_hint: "Изменяются только четыре проверенных поля MAC. Они должны иметь общий префикс и оставаться последовательными в порядке WAN → LAN → RF1 → RF2; все окружающие данные радиокалибровки сохраняются побайтно.",
            params_field: "Поле",
            params_value: "Значение",
            params_add: "Добавить поле",
            params_delete: "Удалить",
            params_save_config: "Сохранить Config",
            params_save_bdata: "Сохранить Bdata",
            params_save_macs: "Сохранить MAC-адреса",
            params_mac_rf1: "MAC WLAN RF1",
            params_mac_rf2: "MAC WLAN RF2",
            params_mac_lan: "MAC LAN",
            params_mac_wan: "MAC WAN",
            params_crc_ok: "CRC проверен",
            params_crc_bad: "CRC неверен — запись отключена",
            params_read_ok: "Успешно прочитано",
            params_read_bad: "Не удалось прочитать",
            params_layout_bad: "Несоответствие разметки — запись отключена",
            params_loading: "Чтение Config, Bdata и Factory…",
            params_saved: "Успешно записано и проверено повторным чтением.",
            params_error: "Не удалось сохранить. Проверьте значения и вывод последовательной консоли.",
            params_invalid: "Имена полей должны быть уникальными и непустыми, значения — корректными.",
            params_mac_invalid: "Используйте четыре последовательных unicast-адреса с одним префиксом в порядке WAN → LAN → RF1 → RF2.",
            params_confirm_env: "Записать эту таблицу параметров целиком? Неверные значения загрузки могут помешать запуску стоковой прошивки Xiaomi.",
            params_confirm_macs: "Записать эти MAC-адреса в Factory? Неверные значения могут нарушить работу сети или привести к постоянному дублированию адресов.",
            params_warn_backup: "Сделайте резервную копию Config, Bdata и Factory перед внесением изменений.",
            params_warn_power: "Не отключайте питание, пока пишется и проверяется блок стирания. Прерывание может оставить NAND в неисправном состоянии.",
            params_warn_stock: "Удаление или изменение полей загрузки может помешать запуску стоковой прошивки Xiaomi.",
            params_warn_factory: "Неверные MAC-адреса могут нарушить работу сети или привести к постоянному дублированию адресов.",
            page_reboot_title: "Перезагрузка",
            h_reboot: "ПЕРЕЗАГРУЗКА УСТРОЙСТВА",
            reboot_info: "Запрос на перезагрузку отправлен. Подождите...<br>Эта страница может ненадолго перестать отвечать.",
            reboot_warn_1: "не отключайте питание устройства во время перезагрузки",
            reboot_confirm: "Перезагрузить устройство сейчас?",
            page_flashing_title: "Идёт обновление",
            h_update_in_progress: "ИДЁТ ОБНОВЛЕНИЕ",
            p_update_in_progress: "Файл успешно загружен! Идёт обновление, дождитесь автоматической перезагрузки устройства.<br>Время обновления зависит от размера образа и может занять несколько минут.",
            h_update_completed: "ОБНОВЛЕНИЕ ЗАВЕРШЕНО",
            p_update_completed: "Устройство успешно обновлено! Идёт перезагрузка...",
            page_booting_title: "Загрузка initramfs",
            h_booting_initramfs: "ЗАГРУЗКА INITRAMFS",
            p_booting_initramfs: "Файл успешно загружен! Идёт загрузка, подождите...<br>Эта страница может ненадолго перестать отвечать.",
            h_boot_success: "ЗАГРУЗКА УСПЕШНА",
            p_boot_success: "Устройство успешно загружено в initramfs!",
            page_fail_title: "Ошибка обновления",
            h_update_failed: "ОШИБКА ОБНОВЛЕНИЯ",
            fail_detail_html: "<strong>Во время обновления что-то пошло не так<\\/strong>Вероятно, выбран неверный файл. Попробуйте ещё раз. Больше сведений о ходе обновления можно увидеть в консоли U-Boot.",
            page_success_title: "Успех",
            h_success: "УСПЕХ",
            p_success: "OK",
            page_404_title: "404 — страница не найдена",
            page_not_found: "Страница не найдена",
            other_warnings: "ПРЕДУПРЕЖДЕНИЯ",
            warn_no_poweroff: "не отключайте питание устройства во время обновления",
            warn_restart: "если всё пройдёт успешно, устройство перезагрузится",
            warn_choose_fw: "загрузить можно любой файл — убедитесь, что выбрали правильный образ прошивки для вашего устройства",
            warn_official_model: "принимается только точный официальный образ R2100/RM2100, соответствующий этому устройству",
            warn_official_layout: "установленная система OpenWrt и оверлей будут стёрты, а калибровка и идентичность устройства сохранены",
            warn_choose_uboot: "загрузить можно любой файл — убедитесь, что выбрали правильный образ U-Boot для вашего устройства",
            warn_uboot_danger: "обновление U-Boot — очень опасная операция и может повредить устройство!",
            warn_boot_initramfs: "если всё пройдёт успешно, устройство загрузится в initramfs",
            warn_choose_initramfs: "загрузить можно любой файл — убедитесь, что выбрали правильный образ initramfs для вашего устройства",
            warn_factory_danger: "обновление раздела Factory может повредить устройство или испортить данные калибровки",
            btn_upload: "Загрузить",
            btn_update: "Обновить",
            btn_restore_official: "Восстановить сейчас",
            btn_boot: "Загрузить",
            prompt_update: "Загрузка завершена. Проверьте сведения о файле перед установкой.",
            prompt_official: "Загрузка завершена. Проверьте модель и сведения о файле перед восстановлением.",
            prompt_boot: "Загрузка завершена. Проверьте сведения о файле перед загрузкой.",
            file_label: "Файл:",
            size_label: "Размер:",
            upload_progress: "Загрузка",
            upload_none: "Сначала выберите файл.",
            upload_error: "Ошибка загрузки. Проверьте кабель и повторите попытку.",
            sysinfo_loading: "Загрузка сведений о системе...",
            sysinfo_unavailable: "Сведения об устройстве недоступны.",
            sysinfo_board: "Плата:",
            sysinfo_ram: "ОЗУ:",
            sysinfo_flash: "Флеш:",
            sysinfo_flash_nmbm: "NMBM:",
            sysinfo_compat: "Совместимость:",
            sysinfo_mtdparts: "mtdparts:",
            sysinfo_mtdids: "mtdids:",
            sysinfo_more: "Подробнее",
            sysinfo_unknown: "неизвестно",
            backup_label_mode: "Режим:",
            backup_label_target: "Цель:",
            backup_label_start: "Начало:",
            backup_label_end: "Конец (не включая):",
            backup_mode_part: "Копия раздела",
            backup_mode_range: "Произвольный диапазон",
            backup_btn_download: "Скачать",
            backup_target_placeholder: "-- выберите --",
            backup_target_full_disk: "Весь флеш",
            backup_range_hint: "Подсказка: поддерживаются десятичные, 0xHEX и суффикс KiB (например, 64KiB).",
            backup_warn_1: "не отключайте питание устройства во время резервного копирования",
            backup_warn_2: "произвольный диапазон читает сырые байты — будьте внимательны со смещениями",
            backup_warn_3: "большие копии могут занять много времени в зависимости от скорости флеша",
            backup_status_starting: "Запуск...",
            backup_status_downloading: "Загрузка:",
            backup_status_preparing: "Подготовка файла...",
            backup_status_done: "Готово:",
            backup_error_no_target: "Выберите цель",
            backup_error_bad_range: "Введите корректные начало/конец",
            backup_error_http: "Ошибка HTTP",
            backup_error_exception: "Ошибка:",
        }
    };

    function load(key, fallback) {
        try { return localStorage.getItem(key) || fallback; }
        catch (e) { return fallback; }
    }

    function save(key, value) {
        try { localStorage.setItem(key, value); }
        catch (e) { }
    }

    function detectLang() {
        var pref = load(STORE_LANG, "auto");
        if (pref === "en" || pref === "zh" || pref === "ru") return pref;
        var code = String(navigator.language || "en").toLowerCase();
        if (code.indexOf("zh") === 0) return "zh";
        if (code.indexOf("ru") === 0) return "ru";
        return "en";
    }

    function t(key) {
        var lang = state.lang || detectLang();
        return (text[lang] && text[lang][key]) || text.en[key] || key;
    }

    function pageName() {
        var path = (location.pathname || "/").toLowerCase();
        if (path === "/" || path.indexOf("index.html") >= 0) return "index";
        if (path.indexOf("official") >= 0) return "official";
        if (path.indexOf("uboot") >= 0) return "uboot";
        if (path.indexOf("initramfs") >= 0) return "initramfs";
        if (path.indexOf("factory") >= 0) return "factory";
        if (path.indexOf("backup") >= 0) return "backup";
        if (path.indexOf("overclock") >= 0) return "overclock";
        if (path.indexOf("params") >= 0) return "params";
        if (path.indexOf("reboot") >= 0) return "reboot";
        if (path.indexOf("flashing") >= 0) return "flashing";
        if (path.indexOf("booting") >= 0) return "booting";
        if (path.indexOf("fail") >= 0) return "fail";
        if (path.indexOf("success") >= 0) return "success";
        return "404";
    }

    function applyTheme(value) {
        var root = document.documentElement;
        state.theme = value === "light" || value === "dark" ? value : "auto";
        if (state.theme === "auto") root.removeAttribute("data-theme");
        else root.setAttribute("data-theme", state.theme);
        var select = document.getElementById("theme-select");
        if (select) select.value = state.theme;
    }

    function setTheme(value) {
        save(STORE_THEME, value);
        applyTheme(value);
    }

    function setLang(value) {
        save(STORE_LANG, value);
        state.lang = value === "en" || value === "zh" || value === "ru" ? value : detectLang();
        document.documentElement.lang = state.lang === "zh" ? "zh-CN" : state.lang;
        applyI18n(document);
        renderSysInfo();
        backupUpdateRangeHint();
        renderOverclock();
        renderParams();
        if (state.backupStatusKey)
            setBackupStatus(t(state.backupStatusKey), state.backupStatusKey);
    }

    function applyI18n(root) {
        var scope = root || document;
        var nodes = scope.querySelectorAll("[data-i18n]");
        var i, node, key;
        for (i = 0; i < nodes.length; i++) {
            node = nodes[i];
            key = node.getAttribute("data-i18n");
            node.textContent = t(key);
        }
        nodes = scope.querySelectorAll("[data-i18n-html]");
        for (i = 0; i < nodes.length; i++) {
            node = nodes[i];
            key = node.getAttribute("data-i18n-html");
            node.innerHTML = t(key);
        }
        nodes = scope.querySelectorAll("[data-i18n-value]");
        for (i = 0; i < nodes.length; i++) {
            node = nodes[i];
            node.value = t(node.getAttribute("data-i18n-value"));
        }
        var titles = {
            index: "page_firmware_title", uboot: "page_uboot_title",
            official: "page_official_title",
            initramfs: "page_initramfs_title", factory: "page_factory_title",
            backup: "page_backup_title", overclock: "page_overclock_title",
            params: "page_params_title",
            reboot: "page_reboot_title",
            flashing: "page_flashing_title", booting: "page_booting_title",
            fail: "page_fail_title", success: "page_success_title",
            "404": "page_404_title"
        };
        document.title = t(titles[pageName()]);
        var langSelect = document.getElementById("lang-select");
        var themeSelect = document.getElementById("theme-select");
        if (langSelect) langSelect.value = load(STORE_LANG, "auto");
        if (themeSelect) themeSelect.value = state.theme;
    }

    function makeLink(href, key) {
        var link = document.createElement("a");
        var dot = document.createElement("span");
        var label = document.createElement("span");
        link.href = href;
        link.className = "nav-link";
        dot.className = "nav-dot";
        label.setAttribute("data-i18n", key);
        link.appendChild(dot);
        link.appendChild(label);
        var path = (location.pathname || "/").toLowerCase();
        var active = href === "/" ? path === "/" || path.indexOf("index.html") >= 0 : path.indexOf(href.slice(1)) >= 0;
        if (active) link.className += " active";
        if (key === "nav_reboot") link.onclick = function () { return confirm(t("reboot_confirm")); };
        return link;
    }

    function makeSection(titleKey, links) {
        var section = document.createElement("div");
        var title = document.createElement("div");
        var i;
        section.className = "nav-section";
        title.className = "nav-section-title";
        title.setAttribute("data-i18n", titleKey);
        section.appendChild(title);
        for (i = 0; i < links.length; i++) section.appendChild(makeLink(links[i][0], links[i][1]));
        return section;
    }

    function enhanceCard(card) {
        if (!card || card.getAttribute("data-modern") === "1") return;
        card.setAttribute("data-modern", "1");
        var head = document.createElement("div");
        var main = document.createElement("div");
        var foot = document.createElement("div");
        var children = [];
        var i, item;
        head.className = "card-head";
        main.className = "card-main";
        foot.className = "card-foot";
        while (card.firstChild) {
            children.push(card.firstChild);
            card.removeChild(card.firstChild);
        }
        for (i = 0; i < children.length; i++) {
            item = children[i];
            if (item.nodeType === 1 && (item.tagName === "H1" || item.id === "hint")) head.appendChild(item);
            else if (item.nodeType === 1 && item.classList.contains("i") && item.classList.contains("w")) foot.appendChild(item);
            else main.appendChild(item);
        }
        card.appendChild(head);
        card.appendChild(main);
        if (foot.children.length) card.appendChild(foot);

        var bar = document.getElementById("bar");
        if (bar && !document.getElementById("bar_text")) {
            var progress = document.createElement("div");
            progress.id = "bar_text";
            progress.className = "bar-text";
            bar.parentNode.insertBefore(progress, bar.nextSibling);
        }
        var file = document.getElementById("file");
        if (file && !document.getElementById("filename")) {
            var filename = document.createElement("div");
            filename.id = "filename";
            filename.style.display = "none";
            var form = document.getElementById("form");
            form.parentNode.insertBefore(filename, form.nextSibling);
        }
    }

    function buildShell() {
        var card = document.getElementById("m");
        if (!card || document.querySelector(".app-shell")) return;
        state.page = state.page || pageName();
        state.lang = detectLang();
        state.theme = load(STORE_THEME, "auto");
        document.documentElement.lang = state.lang === "zh" ? "zh-CN" : "en";
        applyTheme(state.theme);
        document.body.setAttribute("data-page", pageName());

        var shell = document.createElement("div");
        var sidebar = document.createElement("aside");
        var brand = document.createElement("div");
        var mark = document.createElement("span");
        var brandText = document.createElement("span");
        var controls = document.createElement("div");
        var nav = document.createElement("nav");
        var content = document.createElement("main");
        var version = document.getElementById("version");
        shell.className = "app-shell";
        sidebar.className = "app-sidebar";
        brand.className = "sidebar-brand";
        mark.className = "brand-mark";
        mark.textContent = "R";
        brandText.className = "app-brand";
        brandText.setAttribute("data-i18n", "brand");
        brand.appendChild(mark);
        brand.appendChild(brandText);
        sidebar.appendChild(brand);

        controls.className = "app-controls";
        controls.innerHTML = '<label class="app-control-row"><span data-i18n="ctl_language"></span><select id="lang-select" class="app-select"><option value="auto" data-i18n="opt_auto"></option><option value="zh" data-i18n="opt_zh"></option><option value="ru" data-i18n="opt_ru"></option><option value="en" data-i18n="opt_en"></option></select></label><label class="app-control-row"><span data-i18n="ctl_theme"></span><select id="theme-select" class="app-select"><option value="auto" data-i18n="opt_auto"></option><option value="light" data-i18n="opt_light"></option><option value="dark" data-i18n="opt_dark"></option></select></label>';
        sidebar.appendChild(controls);

        nav.className = "app-nav";
        nav.appendChild(makeSection("nav_recovery", [["/", "nav_firmware"], ["/official.html", "nav_official"], ["/uboot.html", "nav_uboot"]]));
        nav.appendChild(makeSection("nav_tools", [["/initramfs.html", "nav_initramfs"], ["/factory.html", "nav_factory"], ["/backup.html", "nav_backup"]]));
        nav.appendChild(makeSection("nav_advanced", [["/overclock.html", "nav_overclock"], ["/params.html", "nav_params"]]));
        nav.appendChild(makeSection("nav_system", [["/reboot.html", "nav_reboot"]]));
        sidebar.appendChild(nav);

        content.className = "app-content";
        enhanceCard(card);
        content.appendChild(card);
        if (version) content.appendChild(version);
        shell.appendChild(sidebar);
        shell.appendChild(content);
        while (document.body.firstChild) document.body.removeChild(document.body.firstChild);
        document.body.appendChild(shell);

        document.getElementById("lang-select").onchange = function () { setLang(this.value); };
        document.getElementById("theme-select").onchange = function () { setTheme(this.value); };
        applyI18n(document);
        setTimeout(function () { document.body.className += " ready"; }, 0);
    }

    window.fsdata = {
        t: t,
        applyI18n: applyI18n,
        ensureUI: buildShell,
        setThemePref: setTheme,
        setLangPref: setLang
    };

    document.addEventListener("DOMContentLoaded", buildShell);
})();

function _t(key) {
    return window.fsdata && window.fsdata.t ? window.fsdata.t(key) : key;
}

function ajax(options) {
    var xhr = window.XMLHttpRequest ? new XMLHttpRequest() : new ActiveXObject("Microsoft.XMLHTTP");
    if (xhr.upload && options.progress) xhr.upload.addEventListener("progress", options.progress);
    xhr.onreadystatechange = function () {
        if (xhr.readyState !== 4) return;
        if (xhr.status === 200) {
            if (options.done) options.done(xhr.responseText);
        } else if (options.error) options.error(xhr.status);
    };
    xhr.onerror = function () { if (options.error) options.error(0); };
    xhr.ontimeout = function () { if (options.error) options.error(0); };
    if (options.timeout) xhr.timeout = options.timeout;
    xhr.open(options.data ? "POST" : "GET", options.url, true);
    xhr.send(options.data || null);
}

function bytesToHuman(value) {
    var n = Number(value || 0), units = ["B", "KiB", "MiB", "GiB"], i = 0;
    while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
    return (n >= 10 || i === 0 ? n.toFixed(0) : n.toFixed(1)) + " " + units[i];
}

function flashName(flash) {
    var name = flash.model || flash.name || "";
    if (flash.model && flash.name && flash.model !== flash.name) name += " [" + flash.name + "]";
    if (flash.size) name += " (" + bytesToHuman(flash.size) + ")";
    return name;
}

function renderSysInfo() {
    var box = document.getElementById("sysinfo"), info, board, ram, mtd, flash, primary, detail, node, details, summary;
    if (!box) return;
    info = window.APP_STATE.sysinfo;
    if (!info) {
        box.textContent = _t(window.APP_STATE.sysinfoError ? "sysinfo_unavailable" : "sysinfo_loading");
        return;
    }
    board = info.board || {};
    ram = info.ram || {};
    mtd = info.mtd || {};
    flash = info.flash || {};
    primary = [];
    detail = [];
    var boardName = board.model || _t("sysinfo_unknown");
    if (board.name && board.name !== board.model) boardName += " [" + board.name + "]";
    primary.push(_t("sysinfo_board") + " " + boardName);
    primary.push(_t("sysinfo_ram") + " " + (ram.size ? bytesToHuman(ram.size) : _t("sysinfo_unknown")));
    if (flash.raw) primary.push(_t("sysinfo_flash") + " " + (flashName(flash.raw) || _t("sysinfo_unknown")));
    if (flash.name || flash.model || flash.size) primary.push((flash.raw ? _t("sysinfo_flash_nmbm") : _t("sysinfo_flash")) + " " + flashName(flash));
    if (board.compatible) detail.push(_t("sysinfo_compat") + " " + board.compatible);
    if (mtd.parts) detail.push(_t("sysinfo_mtdparts") + " " + mtd.parts);
    if (mtd.ids) detail.push(_t("sysinfo_mtdids") + " " + mtd.ids);
    box.innerHTML = "";
    node = document.createElement("div");
    node.textContent = primary.join("\n");
    box.appendChild(node);
    if (detail.length) {
        details = document.createElement("details");
        summary = document.createElement("summary");
        summary.textContent = _t("sysinfo_more");
        details.appendChild(summary);
        node = document.createElement("div");
        node.className = "sysinfo-details-body";
        node.textContent = detail.join("\n");
        details.appendChild(node);
        box.appendChild(details);
    }
}

function getSysInfo() {
    if (!document.getElementById("sysinfo")) return;
    renderSysInfo();
    ajax({
        url: "/sysinfo",
        done: function (body) {
            try {
                window.APP_STATE.sysinfo = JSON.parse(body);
                window.APP_STATE.sysinfoError = false;
            } catch (e) {
                window.APP_STATE.sysinfoError = true;
            }
            renderSysInfo();
        },
        error: function () { window.APP_STATE.sysinfoError = true; renderSysInfo(); }
    });
}

function getversion() {
    if (window.fsdata) window.fsdata.ensureUI();
    var node = document.getElementById("version");
    if (node) node.textContent = "U-Boot 2026.09  源码Yuzhii   适配OpenRouter";
    ajax({ url: "/version", done: function (body) {
        if (node) node.textContent = body;
    }});
}

function startup() {
    if (window.fsdata) window.fsdata.ensureUI();
    getversion();
    getSysInfo();
}

function appInit(page) {
    window.APP_STATE.page = page || "";
    startup();
    if (page === "backup") backupInit();
    if (page === "overclock") overclockInit();
    if (page === "params") paramsInit();
    if (page === "reboot") rebootInit();
}

function formatOverclockRange(info) {
    return _t("oc_range").replace("{min}", info.min_mhz)
        .replace("{max}", info.max_mhz).replace("{step}", info.step_mhz);
}

function setOverclockStatus(key, isError) {
    var node = document.getElementById("oc_status");
    window.APP_STATE.overclockStatusKey = key || "";
    if (!node) return;
    node.textContent = key ? _t(key) : "";
    node.className = "oc-message" + (isError ? " error" : "");
}

function renderOverclock() {
    var info = window.APP_STATE.overclock;
    var current = document.getElementById("oc_current");
    var safe = document.getElementById("oc_safe");
    var saved = document.getElementById("oc_saved");
    var input = document.getElementById("oc_mhz");
    var range = document.getElementById("oc_range");
    if (!current || !safe || !saved || !input || !range || !info) return;
    current.textContent = info.current_mhz + " MHz";
    safe.textContent = info.safe_mhz + " MHz";
    saved.textContent = info.enabled ? info.configured_mhz + " MHz" : _t("oc_disabled");
    input.min = info.min_mhz;
    input.max = info.max_mhz;
    input.step = info.step_mhz;
    input.value = info.enabled ? info.configured_mhz : info.safe_mhz;
    range.textContent = formatOverclockRange(info);
    if (window.APP_STATE.overclockStatusKey)
        setOverclockStatus(window.APP_STATE.overclockStatusKey,
            window.APP_STATE.overclockStatusKey.indexOf("error") >= 0 ||
            window.APP_STATE.overclockStatusKey.indexOf("unavailable") >= 0);
}

function loadOverclock() {
    setOverclockStatus("oc_status_loading", false);
    ajax({
        url: "/cpufreq",
        done: function (body) {
            try {
                var info = JSON.parse(body);
                if (!info.ok) throw new Error("setting unavailable");
                window.APP_STATE.overclock = info;
                window.APP_STATE.overclockStatusKey = "";
                renderOverclock();
                setOverclockStatus("", false);
            } catch (e) { setOverclockStatus("oc_status_unavailable", true); }
        },
        error: function () { setOverclockStatus("oc_status_unavailable", true); }
    });
}

function overclockInit() {
    loadOverclock();
}

function submitOverclock(action, mhz, successKey) {
    var form = new FormData();
    form.append("action", action);
    if (mhz !== undefined) form.append("mhz", String(mhz));
    ajax({
        url: "/cpufreq",
        data: form,
        done: function (body) {
            try {
                var info = JSON.parse(body);
                if (!info.ok) throw new Error(String(info.error || "invalid"));
                window.APP_STATE.overclock = info;
                renderOverclock();
                setOverclockStatus(successKey, false);
            } catch (e) { setOverclockStatus("oc_status_error", true); }
        },
        error: function () { setOverclockStatus("oc_status_error", true); }
    });
}

function saveOverclock() {
    var input = document.getElementById("oc_mhz");
    var value = input && Number(input.value);
    if (!input || !Number.isInteger(value) || !window.confirm(_t("oc_confirm"))) return;
    submitOverclock("set", value, "oc_status_saved");
}

function disableOverclock() {
    if (!window.confirm(_t("oc_disable_confirm"))) return;
    submitOverclock("disable", undefined, "oc_status_disabled");
}

function setParamsStatus(key, isError) {
    var node = document.getElementById("params_status");
    window.APP_STATE.paramsStatusKey = key || "";
    if (!node) return;
    node.textContent = key ? _t(key) : "";
    node.className = "oc-message" + (isError ? " error" : "");
}

function makeParamRow(target, key, value) {
    var row = document.createElement("div");
    var keyInput = document.createElement("input");
    var valueInput = document.createElement("input");
    var remove = document.createElement("button");
    row.className = "param-row";
    keyInput.className = "field-control param-key";
    keyInput.maxLength = 127;
    keyInput.value = key || "";
    keyInput.autocomplete = "off";
    valueInput.className = "field-control param-value";
    valueInput.maxLength = 1023;
    valueInput.value = value || "";
    valueInput.autocomplete = "off";
    remove.type = "button";
    remove.className = "param-delete";
    remove.textContent = _t("params_delete");
    remove.onclick = function () { row.parentNode.removeChild(row); };
    row.appendChild(keyInput);
    row.appendChild(valueInput);
    row.appendChild(remove);
    return row;
}

function addParamRow(target, key, value) {
    var table = document.getElementById("params_" + target + "_rows");
    var row;
    if (!table) return;
    row = makeParamRow(target, key, value);
    table.appendChild(row);
    row.querySelector(".param-key").focus();
}

function renderParamTable(target, section) {
    var table = document.getElementById("params_" + target + "_rows");
    var badge = document.getElementById("params_" + target + "_crc");
    var save = document.getElementById("params_" + target + "_save");
    var entries = section && section.entries || [];
    var i;
    if (!table || !badge || !save) return;
    table.innerHTML = "";
    for (i = 0; i < entries.length; i++)
        table.appendChild(makeParamRow(target, entries[i].key, entries[i].value));
    badge.textContent = section && section.crc_ok ? _t("params_crc_ok") : _t("params_crc_bad");
    badge.className = "param-badge " + (section && section.crc_ok ? "ok" : "bad");
    save.disabled = !(section && section.crc_ok);
}

function renderParams() {
    var info = window.APP_STATE.params;
    var macState = document.getElementById("params_mac_state");
    var macSave = document.getElementById("params_mac_save");
    var ids = ["rf1", "rf2", "lan", "wan"], i, input;
    if (!document.getElementById("params_config_rows") || !info) return;
    renderParamTable("config", info.config);
    renderParamTable("bdata", info.bdata);
    if (macState) {
        macState.textContent = info.macs && !info.macs.read_error ?
            (info.macs.layout_ok ? _t("params_read_ok") : _t("params_layout_bad")) : _t("params_read_bad");
        macState.className = "param-badge " + (info.macs && !info.macs.read_error && info.macs.layout_ok ? "ok" : "bad");
    }
    if (macSave) macSave.disabled = !(info.macs && !info.macs.read_error && info.macs.layout_ok);
    for (i = 0; i < ids.length; i++) {
        input = document.getElementById("mac_" + ids[i]);
        if (!input) continue;
        input.value = info.macs && info.macs[ids[i]] || "";
        input.oninput = function () { this.value = this.value.toUpperCase(); };
    }
    if (window.APP_STATE.paramsStatusKey)
        setParamsStatus(window.APP_STATE.paramsStatusKey,
            window.APP_STATE.paramsStatusKey.indexOf("error") >= 0 ||
            window.APP_STATE.paramsStatusKey.indexOf("invalid") >= 0);
}

function loadParams() {
    setParamsStatus("params_loading", false);
    ajax({
        url: "/params",
        done: function (body) {
            try {
                var info = JSON.parse(body);
                if (!info.ok) throw new Error(String(info.error || "read"));
                window.APP_STATE.params = info;
                window.APP_STATE.paramsStatusKey = "";
                renderParams();
                setParamsStatus("", false);
            } catch (e) { setParamsStatus("params_error", true); }
        },
        error: function () { setParamsStatus("params_error", true); }
    });
}

function paramsInit() {
    loadParams();
}

function serializeParamTable(target) {
    var rows = document.querySelectorAll("#params_" + target + "_rows .param-row");
    var seen = {}, lines = [], i, key, value;
    if (!rows.length) throw new Error("empty");
    for (i = 0; i < rows.length; i++) {
        key = rows[i].querySelector(".param-key").value.trim();
        value = rows[i].querySelector(".param-value").value;
        if (!key || key.indexOf("=") >= 0 || /[\r\n]/.test(key) || /[\r\n]/.test(value) || seen[key])
            throw new Error("invalid");
        seen[key] = true;
        lines.push(encodeURIComponent(key) + "=" + encodeURIComponent(value));
    }
    return lines.join("\n");
}

function submitParams(form, successKey) {
    setParamsStatus("params_loading", false);
    ajax({
        url: "/params",
        data: form,
        done: function (body) {
            try {
                var info = JSON.parse(body);
                window.APP_STATE.params = info;
                if (!info.ok || info.error) throw new Error(String(info.error || "write"));
                window.APP_STATE.paramsStatusKey = successKey;
                renderParams();
                setParamsStatus(successKey, false);
            } catch (e) { setParamsStatus("params_error", true); }
        },
        error: function () { setParamsStatus("params_error", true); }
    });
}

function saveParamEnv(target) {
    var form = new FormData(), entries;
    try { entries = serializeParamTable(target); }
    catch (e) { setParamsStatus("params_invalid", true); return; }
    if (!window.confirm(_t("params_confirm_env"))) return;
    form.append("action", "save_env");
    form.append("target", target);
    form.append("entries", entries);
    submitParams(form, "params_saved");
}

function normalizeMac(text) {
    return String(text || "").trim().toUpperCase();
}

function macIsValid(mac) {
    var first;
    if (!/^([0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(mac)) return false;
    first = parseInt(mac.slice(0, 2), 16);
    return !(first & 1) && mac !== "00:00:00:00:00:00" && mac !== "FF:FF:FF:FF:FF:FF";
}

function saveParamMacs() {
    var ids = ["rf1", "rf2", "lan", "wan"], values = {}, seen = {}, i, value;
    var form = new FormData();
    for (i = 0; i < ids.length; i++) {
        value = normalizeMac(document.getElementById("mac_" + ids[i]).value);
        if (!macIsValid(value) || seen[value]) {
            setParamsStatus("params_mac_invalid", true);
            return;
        }
        values[ids[i]] = value;
        seen[value] = true;
    }
    var prefix = values.wan.slice(0, 15);
    var last = parseInt(values.wan.slice(15), 16);
    if (last > 0xFC || values.lan !== prefix + (last + 1).toString(16).padStart(2, "0").toUpperCase() ||
        values.rf1 !== prefix + (last + 2).toString(16).padStart(2, "0").toUpperCase() ||
        values.rf2 !== prefix + (last + 3).toString(16).padStart(2, "0").toUpperCase()) {
        setParamsStatus("params_mac_invalid", true);
        return;
    }
    if (!window.confirm(_t("params_confirm_macs"))) return;
    form.append("action", "save_macs");
    for (i = 0; i < ids.length; i++) form.append(ids[i], values[ids[i]]);
    submitParams(form, "params_saved");
}

function upload(field) {
    var fileInput = document.getElementById("file");
    var file = fileInput && fileInput.files[0];
    if (!file) { alert(_t("upload_none")); return; }
    var form = new FormData();
    var progress = document.getElementById("bar");
    var progressText = document.getElementById("bar_text");
    form.append(field, file);
    document.getElementById("form").style.display = "none";
    var hint = document.getElementById("hint");
    if (hint) hint.style.display = "none";
    ajax({
        url: "/upload",
        data: form,
        done: function (body) {
            if (body === "fail") { location = "/fail.html"; return; }
            var parts = body.trim().split(/\s+/);
            var filename = document.getElementById("filename");
            if (filename) { filename.style.display = "block"; filename.textContent = _t("file_label") + " " + file.name; }
            var size = document.getElementById("size");
            var md5 = document.getElementById("md5");
            size.style.display = "block";
            size.textContent = _t("size_label") + " " + bytesToHuman(parts[0]) + " (" + parts[0] + " B)";
            md5.style.display = "block";
            md5.textContent = "MD5: " + parts[1];
            document.getElementById("upgrade").style.display = "block";
            if (progressText) progressText.textContent = "100%";
        },
        error: function () { alert(_t("upload_error")); location = "/fail.html"; },
        progress: function (event) {
            if (!event.total) return;
            var percent = Math.floor(event.loaded * 100 / event.total);
            progress.style.setProperty("--percent", percent);
            progress.style.display = "block";
            if (progressText) { progressText.style.display = "block"; progressText.textContent = _t("upload_progress") + " " + percent + "%"; }
        }
    });
}

function setBackupStatus(message, key) {
    var node = document.getElementById("backup_status");
    window.APP_STATE.backupStatusKey = key || "";
    if (node) { node.style.display = message ? "block" : "none"; node.textContent = message; }
}

function setBackupProgress(value) {
    var bar = document.getElementById("bar"), n = Math.max(0, Math.min(100, parseInt(value || 0, 10)));
    if (bar) { bar.style.display = "block"; bar.style.setProperty("--percent", n); }
    var label = document.getElementById("bar_text");
    if (label) { label.style.display = "block"; label.textContent = n + "%"; }
}

function backupUpdateRangeHint() {
    var node = document.getElementById("backup_range_hint");
    if (node) node.textContent = _t("backup_range_hint");
}

function selectFirstBackupTarget(select, preferPartition) {
    var i;
    if (!select || select.value) return;
    for (i = 0; i < select.options.length; i++) {
        if (!select.options[i].value) continue;
        if (!preferPartition || select.options[i].value.indexOf("mtd:") === 0) {
            select.selectedIndex = i;
            return;
        }
    }
    if (preferPartition) selectFirstBackupTarget(select, false);
}

function backupInit() {
    var mode = document.getElementById("backup_mode");
    var range = document.getElementById("backup_range");
    var target = document.getElementById("backup_target");
    if (!mode || !range || !target) return;
    function updateMode() {
        var custom = mode.value === "range";
        range.style.display = custom ? "block" : "none";
        var pair = document.getElementById("backup_target_pair");
        if (pair) pair.style.display = custom ? "none" : "flex";
        if (custom) selectFirstBackupTarget(target, true);
        backupUpdateRangeHint();
    }
    mode.onchange = updateMode;
    updateMode();
    setBackupStatus("");
    ajax({
        url: "/backupinfo",
        done: function (body) {
            var info, option, i;
            try { info = JSON.parse(body); }
            catch (e) { setBackupStatus(_t("backup_error_exception") + " invalid device information"); return; }
            window.APP_STATE.backupinfo = info;
            target.innerHTML = "";
            option = document.createElement("option");
            option.value = "";
            option.textContent = _t("backup_target_placeholder");
            target.appendChild(option);
            if (info.mtd && info.mtd.devices) {
                for (i = 0; i < info.mtd.devices.length; i++) {
                    var device = info.mtd.devices[i];
                    if (!device || !device.name) continue;
                    option = document.createElement("option");
                    option.value = "mtddev:" + device.name;
                    option.textContent = "[MTD] " + _t("backup_target_full_disk") + " " + device.name + (device.size ? " (" + bytesToHuman(device.size) + ")" : "");
                    target.appendChild(option);
                }
            }
            if (info.mtd && info.mtd.parts) {
                for (i = 0; i < info.mtd.parts.length; i++) {
                    var part = info.mtd.parts[i];
                    if (!part || !part.name) continue;
                    option = document.createElement("option");
                    option.value = "mtd:" + part.name;
                    option.textContent = "[MTD] " + part.name + (part.size ? " (" + bytesToHuman(part.size) + ")" : "");
                    target.appendChild(option);
                }
            }
            selectFirstBackupTarget(target, true);
        },
        error: function () { setBackupStatus(_t("sysinfo_unavailable"), "sysinfo_unavailable"); }
    });
}

function parseFilename(disposition) {
    var match = disposition && /filename\*=UTF-8''([^;]+)|filename="?([^";]+)"?/i.exec(disposition);
    if (!match) return "backup.bin";
    try { return decodeURIComponent(match[1] || match[2]); }
    catch (e) { return match[1] || match[2]; }
}

function safeName(value) {
    return String(value || "board").replace(/[^a-zA-Z0-9._-]+/g, "_").replace(/^_+|_+$/g, "").slice(0, 48) || "board";
}

function backupFilename(original) {
    var board = window.APP_STATE.sysinfo && window.APP_STATE.sysinfo.board;
    var model = safeName(board && board.model);
    var now = new Date();
    var date = String(now.getFullYear()) + String(now.getMonth() + 1).padStart(2, "0") + String(now.getDate()).padStart(2, "0");
    var name = String(original || "backup.bin").replace(/^backup_/, "");
    if (!/\.[A-Za-z0-9]+$/.test(name)) name += ".bin";
    return "backup_" + model + "_" + name.replace(/(\.[A-Za-z0-9]+)$/, "_" + date + "$1");
}

async function startBackup() {
    var mode = document.getElementById("backup_mode");
    var target = document.getElementById("backup_target");
    if (!mode || !target) return;
    selectFirstBackupTarget(target, mode.value === "range");
    if (!target.value) { alert(_t("backup_error_no_target")); return; }
    var form = new FormData();
    form.append("mode", mode.value);
    form.append("target", target.value);
    if (mode.value === "range") {
        var start = document.getElementById("backup_start");
        var end = document.getElementById("backup_end");
        if (!start.value || !end.value) { alert(_t("backup_error_bad_range")); return; }
        form.append("start", start.value);
        form.append("end", end.value);
    }
    setBackupProgress(0);
    setBackupStatus(_t("backup_status_starting"));
    try {
        var response = await fetch("/backup", { method: "POST", body: form });
        if (!response.ok) throw new Error(_t("backup_error_http") + " " + response.status);
        var total = Number(response.headers.get("Content-Length") || 0);
        var name = backupFilename(parseFilename(response.headers.get("Content-Disposition")));
        var reader = response.body.getReader();
        var chunks = [], loaded = 0, item, fileStream = null;
        if (window.showSaveFilePicker) {
            try {
                var handle = await window.showSaveFilePicker({
                    suggestedName: name,
                    types: [{
                        description: "Raw flash image",
                        accept: { "application/octet-stream": [".bin"] }
                    }]
                });
                fileStream = await handle.createWritable();
            } catch (e) {
                if (e && e.name === "AbortError") {
                    try { await reader.cancel(); } catch (cancelError) { }
                    setBackupStatus("");
                    return;
                }
                fileStream = null;
            }
        }
        while (true) {
            item = await reader.read();
            if (item.done) break;
            if (fileStream) await fileStream.write(item.value);
            else chunks.push(item.value);
            loaded += item.value.length;
            if (total) setBackupProgress(loaded * 100 / total);
            setBackupStatus(_t("backup_status_downloading") + " " + bytesToHuman(loaded) + (total ? " / " + bytesToHuman(total) : ""));
        }
        if (fileStream) {
            await fileStream.close();
        } else {
            setBackupStatus(_t("backup_status_preparing"));
            var link = document.createElement("a");
            var objectUrl = URL.createObjectURL(new Blob(chunks, { type: "application/octet-stream" }));
            link.href = objectUrl;
            link.download = name;
            document.body.appendChild(link);
            link.click();
            document.body.removeChild(link);
            setTimeout(function () { URL.revokeObjectURL(objectUrl); }, 1000);
        }
        setBackupProgress(100);
        setBackupStatus(_t("backup_status_done") + " " + name);
    } catch (e) {
        setBackupStatus(_t("backup_error_exception") + " " + (e.message || e));
    }
}

function rebootInit() {
    fetch("/reboot", { method: "GET" }).catch(function () { });
}
