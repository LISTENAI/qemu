rom_dir = get_option('lisem_venusa_rom_dir')
if rom_dir == '' or not fs.is_absolute(rom_dir)
  error('VenusA requires an absolute -Dlisem_venusa_rom_dir')
endif
rom_header = custom_target('venusa-rom-image',
  input: [rom_dir / 'manifest.json', rom_dir / 'ap.bin'],
  output: 'venusa_rom.inc',
  command: [python, files('../../../scripts/lisem-assets.py'), 'venusa-rom',
            '--directory', rom_dir, '--output', '@OUTPUT@'])
venusa_assets = declare_dependency(sources: rom_header,
  include_directories: include_directories('.'))
riscv_ss.add(when: 'CONFIG_VENUSA', if_true: [venusa_assets, files(
  'venusa.c', 'venusa_luna.c', 'venusa_audio.c', 'venusa_pcm.c',
  'venusa_dvp.c', 'venusa_camera.c', 'venusa_touch.c', 'venusa_image.c', 'venusa_soc.c', 'venusa_spi.c', 'venusa_uart.c', 'venusa_storage.c',
  'venusa_flashdl.c', 'venusa_psram.c', 'venusa_sysctl.c', 'venusa_gpio.c',
  'venusa_runtime.c', 'venusa_peripherals.c', 'venusa_gpt.c', 'venusa_aon.c',
)])
