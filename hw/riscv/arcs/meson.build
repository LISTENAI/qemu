rom_dir = get_option('lisem_rom_dir')
luna_dir = get_option('lisem_luna_dir')
if rom_dir == '' or luna_dir == ''
  error('ARCS requires -Dlisem_rom_dir and -Dlisem_luna_dir')
endif
if not fs.is_absolute(rom_dir) or not fs.is_absolute(luna_dir)
  error('Lisem asset directories must be absolute paths')
endif

asset_tool = files('../../../scripts/lisem-assets.py')
luna_api = files('../../../include/lisem/luna.h')
luna_archive = luna_dir / 'liblisem-luna.a'
luna_manifest = luna_dir / 'manifest.json'
luna_check = custom_target('lisem-luna-validation',
  input: [luna_archive, luna_manifest, luna_api],
  output: 'lisem_luna_validated.h',
  command: [python, asset_tool, 'luna', '--directory', luna_dir,
            '--api', luna_api, '--target',
            host_machine.system() + '-' + host_machine.cpu_family(),
            '--output', '@OUTPUT@'])
rom_header = custom_target('arcs-rom-images',
  input: [rom_dir / 'manifest.json', rom_dir / 'ap.bin', rom_dir / 'cp.bin'],
  output: 'arcs_roms.inc',
  command: [python, asset_tool, 'rom', '--directory', rom_dir,
            '--output', '@OUTPUT@'])

arcs_sources = files(
  'arcs.c', 'arcs_soc.c', 'arcs_uart.c', 'arcs_gpio.c',
  'arcs_sysctl.c', 'arcs_psram.c', 'arcs_storage.c', 'arcs_dma.c',
  'arcs_spi.c', 'arcs_gpt.c', 'arcs_mailbox.c', 'arcs_luna.c', 'arcs_aux.c',
  'arcs_usb.c', 'arcs_usb_adb.c', 'arcs_i2c.c', 'arcs_dvp.c',
  'arcs_rf.c', 'arcs_wifi.c', 'arcs_wifi_dma.c', 'arcs_aon_timer.c',
  'arcs_aon_wdt.c', 'arcs_dual_timer.c', 'arcs_bluetooth.c',
  'arcs_audio.c', 'arcs_dma2d.c', 'arcs_wifi_tx.c', 'arcs_wifi_rx.c',
  'arcs_wifi_ap.c', 'arcs_wifi_security.c', 'arcs_hsu.c',
  'arcs_aes.c', 'arcs_trng.c', 'arcs_network.c', 'arcs_jpeg.c',
  '../../i2c/gc0328.c', '../../i2c/cst328.c',
)
arcs_assets = declare_dependency(
  sources: [rom_header, luna_check],
  include_directories: include_directories('.'),
  link_args: [luna_archive],
)
riscv_ss.add(when: 'CONFIG_ARCS', if_true: [
  arcs_sources, arcs_assets, dependency('gmodule-2.0'), dependency('libjpeg'),
])
