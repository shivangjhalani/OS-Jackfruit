#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/export-internal.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

#ifdef CONFIG_UNWINDER_ORC
#include <asm/orc_header.h>
ORC_HEADER;
#endif

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x81daace6, "cdev_init" },
	{ 0x6a7b86fa, "cdev_add" },
	{ 0xc6f46339, "init_timer_key" },
	{ 0x15ba50a6, "jiffies" },
	{ 0xc38c83b8, "mod_timer" },
	{ 0x122c3a7e, "_printk" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0xe9b868f, "device_destroy" },
	{ 0xeea0e0d, "class_destroy" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0x4c03a563, "random_kmalloc_seed" },
	{ 0x1004e946, "kmalloc_caches" },
	{ 0xbf55f104, "kmalloc_trace" },
	{ 0x9166fada, "strncpy" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0x5a921311, "strncmp" },
	{ 0x37a0cba, "kfree" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x82ee90dc, "timer_delete_sync" },
	{ 0x67d01ca4, "cdev_del" },
	{ 0xe2b4a000, "mmput" },
	{ 0x8d522714, "__rcu_read_lock" },
	{ 0x20640ee0, "find_vpid" },
	{ 0x174903c0, "pid_task" },
	{ 0x2469810f, "__rcu_read_unlock" },
	{ 0x56ef94db, "get_task_mm" },
	{ 0xb8950865, "__put_task_struct" },
	{ 0x296695f, "refcount_warn_saturate" },
	{ 0xb5326d94, "send_sig" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
	{ 0xa4bf0f83, "class_create" },
	{ 0x31ba63ba, "device_create" },
	{ 0x73776b79, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "534D94BE1D1636D4292DD64");
