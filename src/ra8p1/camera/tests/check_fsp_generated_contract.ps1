$ErrorActionPreference = "Stop"

$cameraRoot = Split-Path -Parent $PSScriptRoot
$configurationPath = Join-Path $cameraRoot "configuration.xml"
$halDataPath = Join-Path $cameraRoot "ra_gen\hal_data.c"
$pinDataPath = Join-Path $cameraRoot "ra_gen\pin_data.c"

[xml] $configuration = Get-Content -LiteralPath $configurationPath -Raw
$ceuModule = $configuration.SelectSingleNode(
    "//module[starts-with(@id, 'module.driver.ceu_on_capture.')]"
)
if ($null -eq $ceuModule) {
    throw "FSP contract failed: CEU module is missing from configuration.xml"
}

$bytesPerPixel = $ceuModule.SelectSingleNode(
    "property[@id='module.driver.ceu.bytes_per_pixel']"
)
if (($null -eq $bytesPerPixel) -or ($bytesPerPixel.value -ne "1")) {
    throw ("FSP contract failed: CEU bytes_per_pixel must be explicitly set to 1; " +
           "FSP 6.5.0 silently defaults the hidden property to 2 when it is absent")
}

$halData = Get-Content -LiteralPath $halDataPath -Raw
$requiredGeneratedPatterns = @(
    "\.x_capture_pixels\s*=\s*1280\s*,",
    "\.y_capture_pixels\s*=\s*480\s*,",
    "\.bytes_per_pixel\s*=\s*1\s*,",
    "\.image_area_size\s*=\s*1280\s*\*\s*480\s*\*\s*1\s*,"
)

foreach ($pattern in $requiredGeneratedPatterns) {
    if ($halData -notmatch $pattern) {
        throw "FSP contract failed: generated hal_data.c does not match VGA 8-bit CEU capture ($pattern)"
    }
}

$blockMediaModule = $configuration.SelectSingleNode(
    "//module[starts-with(@id, 'module.driver.rm_block_media_on_rm_block_media_sdmmc.')]"
)
$sdmmcModule = $configuration.SelectSingleNode(
    "//module[starts-with(@id, 'module.driver.sdmmc_on_sdmmc.')]"
)
$dmacModule = $configuration.SelectSingleNode(
    "//module[starts-with(@id, 'module.driver.transfer_on_dmac.')]"
)
if (($null -eq $blockMediaModule) -or ($null -eq $sdmmcModule) -or
    ($null -eq $dmacModule)) {
    throw "FSP contract failed: SDHI0 block-media/SDMMC/DMAC0 stack is incomplete"
}

$requiredModuleProperties = @(
    @($blockMediaModule, "module.driver.rm_block_media.name", "g_sd_block_media"),
    @($blockMediaModule, "module.driver.rm_block_media.callback", "sd_media_callback"),
    @($sdmmcModule, "module.driver.sdmmc.name", "g_sdmmc0"),
    @($sdmmcModule, "module.driver.sdmmc.channel", "0"),
    @($sdmmcModule, "module.driver.sdmmc.bus_width", "module.driver.sdmmc.bus_width.bus_width_4_bits"),
    @($sdmmcModule, "module.driver.sdmmc.card_detect", "module.driver.sdmmc.card_detect.card_detect_cd"),
    @($sdmmcModule, "module.driver.sdmmc.p_callback", "rm_block_media_sdmmc_callback"),
    @($dmacModule, "module.driver.transfer.name", "g_sdhi_transfer"),
    @($dmacModule, "module.driver.transfer.channel", "0"),
    @($dmacModule, "module.driver.transfer.activation_event", "_signal.event.sdhimmc0.dma.req"),
    @($dmacModule, "module.driver.transfer.p_callback", "g_sdmmc0_dmac_callback")
)
foreach ($requirement in $requiredModuleProperties) {
    $property = $requirement[0].SelectSingleNode("property[@id='$($requirement[1])']")
    if (($null -eq $property) -or ($property.value -ne $requirement[2])) {
        throw "FSP contract failed: $($requirement[1]) must be $($requirement[2])"
    }
}

$requiredPinAssignments = @{
    "sdhi0.sd0dat3" = "sdhi0.sd0dat3.p111"
    "sdhi0.sd0dat2" = "sdhi0.sd0dat2.pd01"
    "sdhi0.sd0dat1" = "sdhi0.sd0dat1.pd02"
    "sdhi0.sd0dat0" = "sdhi0.sd0dat0.pd03"
    "sdhi0.sd0cmd"  = "sdhi0.sd0cmd.pd04"
    "sdhi0.sd0clk"  = "sdhi0.sd0clk.pd05"
    "sdhi0.sd0cd"   = "sdhi0.sd0cd.pd07"
}
foreach ($configurationId in $requiredPinAssignments.Keys) {
    $setting = $configuration.SelectSingleNode(
        "//configSetting[@configurationId='$configurationId']"
    )
    if (($null -eq $setting) -or
        ($setting.altId -ne $requiredPinAssignments[$configurationId])) {
        throw "FSP contract failed: $configurationId must use $($requiredPinAssignments[$configurationId])"
    }
}
if ($null -ne $configuration.SelectSingleNode("//configSetting[@configurationId='pd06']")) {
    throw "FSP contract failed: PD06 must remain unassigned for the reserved GPT2 PWM"
}

$requiredSdGeneratedPatterns = @(
    "const transfer_instance_t g_sdhi_transfer",
    "\.channel\s*=\s*0\s*,",
    "\.activation_source\s*=\s*ELC_EVENT_SDHIMMC0_DMA_REQ\s*,",
    "\.bus_width\s*=\s*SDMMC_BUS_WIDTH_4_BITS\s*,",
    "\.card_detect\s*=\s*SDMMC_CARD_DETECT_CD\s*,",
    "\.p_lower_lvl_transfer\s*=\s*&g_sdhi_transfer\s*,",
    "\.p_callback\s*=\s*sd_media_callback\s*,"
)
foreach ($pattern in $requiredSdGeneratedPatterns) {
    if ($halData -notmatch $pattern) {
        throw "FSP contract failed: generated hal_data.c is missing SDHI0/DMAC0 contract ($pattern)"
    }
}

$pinData = Get-Content -LiteralPath $pinDataPath -Raw
$requiredGeneratedPins = @(
    "BSP_IO_PORT_01_PIN_11",
    "BSP_IO_PORT_13_PIN_01",
    "BSP_IO_PORT_13_PIN_02",
    "BSP_IO_PORT_13_PIN_03",
    "BSP_IO_PORT_13_PIN_04",
    "BSP_IO_PORT_13_PIN_05",
    "BSP_IO_PORT_13_PIN_07"
)
foreach ($pin in $requiredGeneratedPins) {
    $pattern = "(?s)\.pin\s*=\s*$pin\s*,.{0,240}IOPORT_PERIPHERAL_SDHI_MMC"
    if ($pinData -notmatch $pattern) {
        throw "FSP contract failed: generated pin_data.c does not assign $pin to SDHI/MMC"
    }
}
if ($pinData -match "BSP_IO_PORT_13_PIN_06") {
    throw "FSP contract failed: generated pin_data.c unexpectedly assigns reserved PD06"
}

Write-Output ("fsp-generated-contract status=ok ceu=1280x480x1 frame_bytes=614400 " +
              "sdhi0=4bit pins=P111/PD01-PD05/PD07 transfer=DMAC0 pd06=free")
