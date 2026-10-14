rom_dir = get_option('lisem_rom_dir')
if rom_dir == '' or not fs.is_absolute(rom_dir)
  error('ARCS requires an absolute -Dlisem_rom_dir')
endif
asset_tool = files('../../../scripts/lisem-assets.py')
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
  '../../i2c/cst328.c',
)
arcs_assets = declare_dependency(
  sources: [rom_header],
  include_directories: include_directories('.'),
)
riscv_ss.add(when: 'CONFIG_ARCS', if_true: [
  arcs_sources, arcs_assets, dependency('gmodule-2.0'), dependency('libjpeg'),
])
