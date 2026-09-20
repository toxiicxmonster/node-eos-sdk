{
  "variables": {
    "openssl_fips": ""
  },
  "targets": [
    {
      "target_name": "eos",
      "sources": [
        "src/addon.cc",
        "src/platform.cc",
        "src/connect.cc",
        "src/lobby.cc",
        "src/p2p.cc",
        "src/callbacks.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "vendor/eos/include"
      ],
      "defines": [
        "NAPI_VERSION=8",
        "NAPI_DISABLE_CPP_EXCEPTIONS"
      ],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "cflags_cc": ["-std=c++17"],
      "conditions": [
        ["OS=='win'", {
          "libraries": ["../vendor/eos/lib/win64/EOSSDK-Win64-Shipping.lib"],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "AdditionalOptions": ["/std:c++17", "/EHsc"],
              "ExceptionHandling": 1
            }
          },
          "copies": [
            {
              "destination": "<(module_root_dir)/build/Release/",
              "files": ["<(module_root_dir)/vendor/eos/lib/win64/EOSSDK-Win64-Shipping.dll"]
            }
          ]
        }],
        ["OS=='mac'", {
          "libraries": ["../vendor/eos/lib/osx/libEOSSDK-Mac-Shipping.dylib"],
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "CLANG_CXX_LIBRARY": "libc++",
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "MACOSX_DEPLOYMENT_TARGET": "10.15",
            "OTHER_LDFLAGS": ["-Wl,-rpath,@loader_path"]
          },
          "copies": [
            {
              "destination": "<(module_root_dir)/build/Release/",
              "files": ["<(module_root_dir)/vendor/eos/lib/osx/libEOSSDK-Mac-Shipping.dylib"]
            }
          ]
        }],
        ["OS=='linux'", {
          "libraries": ["../vendor/eos/lib/linux/libEOSSDK-Linux-Shipping.so"],
          "ldflags": ["-Wl,-rpath,'$$ORIGIN'"],
          "copies": [
            {
              "destination": "<(module_root_dir)/build/Release/",
              "files": ["<(module_root_dir)/vendor/eos/lib/linux/libEOSSDK-Linux-Shipping.so"]
            }
          ]
        }]
      ]
    }
  ]
}
