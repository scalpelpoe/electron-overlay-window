{
  'variables': {
    'wayland_host%': '<!(node -e "try{require(\'child_process\').execSync(\'pkg-config --exists wayland-client wayland-cursor xkbcommon wayland-scanner\');console.log(process.platform===\'linux\'?1:0)}catch{console.log(0)}")',
    # Optional: KWin window tracking needs libdbus, which Electron links anyway.
    'wayland_dbus%': '<!(node -e "try{require(\'child_process\').execSync(\'pkg-config --exists dbus-1\');console.log(1)}catch{console.log(0)}")'
  },
  'targets': [
    {
      'target_name': 'overlay_window',
      'sources': [
        'src/lib/addon.c',
        'src/lib/napi_helpers.c'
      ],
      'include_dirs': [
        'src/lib'
      ],
      'conditions': [
        ['OS=="win"', {
          'defines': [
            'WIN32_LEAN_AND_MEAN'
          ],
          'link_settings': {
            'libraries': [
              'oleacc.lib'
            ]
          },
      	  'sources': [
            'src/lib/windows.c',
          ]
      	}],
        ['OS=="linux"', {
          'defines': [
            '_GNU_SOURCE'
          ],
          'link_settings': {
            'libraries': [
              '-lxcb', '-lpthread'
            ]
          },
          'cflags': ['-std=c99', '-pedantic', '-Wall', '-pthread'],
      	  'sources': [
            'src/lib/x11.c',
          ]
        }],
        ['OS=="mac"', {
          'link_settings': {
            'libraries': [
              '-lpthread', '-framework AppKit', '-framework ApplicationServices'
            ]
          },
          'xcode_settings': {
            'OTHER_CFLAGS': [
              '-fobjc-arc'
            ]
          },
          'cflags': ['-std=c99', '-pedantic', '-Wall', '-pthread'],
          'sources': [
            'src/lib/mac.mm',
            'src/lib/mac/OWFullscreenObserver.mm'
          ]
        }]
      ]
    }
  ],
  'conditions': [
    # Electron statically links its own libwayland-client and exports its
    # symbols, so Wayland code cannot live inside the addon: the dynamic linker
    # would bind part of it to Electron's copy. The layer-shell host is a
    # separate executable instead. See src/lib/wayland/README.md.
    ['OS=="linux" and wayland_host==1', {
      'targets': [
        {
          'target_name': 'overlay-wayland-host',
          'type': 'executable',
          'defines': [
            '_GNU_SOURCE'
          ],
          'include_dirs': [
            'src/lib/wayland',
            '<(SHARED_INTERMEDIATE_DIR)/wayland-protocols'
          ],
          'cflags': ['-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter'],
          'sources': [
            'src/lib/wayland/host.c',
            'src/lib/wayland/protocols/cursor-shape-v1.xml',
            'src/lib/wayland/protocols/ext-data-control-v1.xml',
            'src/lib/wayland/protocols/tablet-v2.xml',
            'src/lib/wayland/protocols/viewporter.xml',
            'src/lib/wayland/protocols/virtual-keyboard-unstable-v1.xml',
            'src/lib/wayland/protocols/wlr-data-control-unstable-v1.xml',
            'src/lib/wayland/protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
            'src/lib/wayland/protocols/wlr-layer-shell-unstable-v1.xml',
            'src/lib/wayland/protocols/wlr-screencopy-unstable-v1.xml',
            'src/lib/wayland/protocols/wlr-virtual-pointer-unstable-v1.xml',
            'src/lib/wayland/protocols/xdg-output-unstable-v1.xml',
            'src/lib/wayland/protocols/xdg-shell.xml'
          ],
          # Protocol glue is generated from the vendored XML at build time.
          'rules': [
            {
              'rule_name': 'wayland-scanner',
              'extension': 'xml',
              'outputs': [
                '<(SHARED_INTERMEDIATE_DIR)/wayland-protocols/<(RULE_INPUT_ROOT)-client-protocol.h',
                '<(SHARED_INTERMEDIATE_DIR)/wayland-protocols/<(RULE_INPUT_ROOT)-protocol.c'
              ],
              'action': [
                'sh', '-c',
                'wayland-scanner client-header <(RULE_INPUT_PATH) <(SHARED_INTERMEDIATE_DIR)/wayland-protocols/<(RULE_INPUT_ROOT)-client-protocol.h && '
                'wayland-scanner private-code <(RULE_INPUT_PATH) <(SHARED_INTERMEDIATE_DIR)/wayland-protocols/<(RULE_INPUT_ROOT)-protocol.c'
              ],
              'message': 'Generating <(RULE_INPUT_ROOT) protocol code',
              'process_outputs_as_sources': 1
            }
          ],
          'link_settings': {
            'libraries': [
              '<!@(pkg-config --libs wayland-client wayland-cursor xkbcommon)'
            ]
          },
          'conditions': [
            ['wayland_dbus==1', {
              'defines': [
                'HAVE_DBUS'
              ],
              'cflags': ['<!@(pkg-config --cflags dbus-1)'],
              'sources': [
                'src/lib/wayland/bus.c',
                'src/lib/wayland/kwin.c'
              ],
              'link_settings': {
                'libraries': [
                  '<!@(pkg-config --libs dbus-1)'
                ]
              }
            }]
          ]
        }
      ]
    }]
  ]
}
