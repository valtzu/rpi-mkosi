```plantuml


file sign_key_private.pem



rectangle esp {
    artifact boot.img {
        file fixup4.dat
        file start4.elf
        file config.txt
        artifact RPI_EFI.fd {
        }
        artifact pieeprom.upd {
            file sign_key_public.pem
            file boot_conf.txt
            artifact boot_conf.sig
        }
        artifact pieeprom.sig {
        }
    }
    artifact boot.sig
    
    artifact "EFI/Linux/system_0.2.0.efi" as uki {
        artifact initrd
        artifact cmdline
        artifact vmlinux
        artifact dtb
        note bottom
        Not here yet
        end note
    }
}

sign_key_private.pem -.> sign_key_public.pem
boot.img .up-> boot.sig
sign_key_private.pem .> boot.sig

boot_conf.txt -.-> boot_conf.sig
pieeprom.upd -up.> pieeprom.sig
sign_key_private.pem -.-> boot_conf.sig
sign_key_private.pem -.-> pieeprom.sig
sign_key_private.pem .-> uki
sign_key_public.pem -> RPI_EFI.fd

```
