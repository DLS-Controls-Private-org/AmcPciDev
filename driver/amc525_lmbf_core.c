#include <linux/module.h>
#include <linux/version.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include "error.h"


#define DEVICE_NAME     "amc525_lamc_priv"

MODULE_AUTHOR("Michael Abbott, Diamond Light Source Ltd.");
MODULE_DESCRIPTION("Driver for LAMC525 FPGA MTCA card");
MODULE_LICENSE("GPL");
// MODULE_VERSION(S(VERSION));
MODULE_VERSION("0");

#define XILINX_VID      0x10EE
#define AMC525_DID      0x7038


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* All the driver specific state for a card is in this structure. */
struct amc525_lamc_priv {
    struct cdev cdev;
    struct pci_dev *dev;
    int board;              // Index number for this board
    int minor;              // Associated minor number

    unsigned long reg_length;
    void __iomem *reg_memory;
};


static int lamc_pci_reg_map(struct file *file, struct vm_area_struct *vma)
{
    struct amc525_lamc_priv *lamc_priv = file->private_data;

    size_t size = vma->vm_end - vma->vm_start;
    unsigned long end = (vma->vm_pgoff << PAGE_SHIFT) + size;
    if (end > lamc_priv->reg_length)
    {
        printk(KERN_WARNING DEVICE_NAME " map area out of range\n");
        return -EINVAL;
    }

    /* Good advice and examples on using this function here:
     *  http://www.makelinux.net/ldd3/chp-15-sect-2
     * Also see drivers/char/mem.c in kernel sources for guidelines. */
    unsigned long base_page = pci_resource_start(lamc_priv->dev, 0) >> PAGE_SHIFT;
    return io_remap_pfn_range(
        vma, vma->vm_start, base_page + vma->vm_pgoff, size,
        pgprot_noncached(vma->vm_page_prot));
}


static struct file_operations lamc_pci_reg_fops = {
    .owner = THIS_MODULE,
    .mmap = lamc_pci_reg_map,
};

static struct file_operations lamc_pci_mem_fops = {
    .owner = THIS_MODULE,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Basic file operations. */

static struct {
    const char *name;
    struct file_operations *fops;
} fops_info[] = {
    { .name = "reg",    .fops = &lamc_pci_reg_fops, },
    { .name = "mem",    .fops = &lamc_pci_mem_fops, },
};

#define MINORS_PER_BOARD    ARRAY_SIZE(fops_info)


static int amc525_lamc_pci_open(struct inode *inode, struct file *file)
{
    /* Recover our private data: the i_cdev lives inside our private structure,
     * so we'll copy the appropriate link to our file structure. */
    struct cdev *cdev = inode->i_cdev;
    struct amc525_lamc_priv *lamc_priv = container_of(cdev, struct amc525_lamc_priv, cdev);
    file->private_data = lamc_priv;


    /* Replace the file's f_ops with our own. */
    int minor_index = iminor(inode) - lamc_priv->minor;
    if (0 <= minor_index  &&  minor_index < MINORS_PER_BOARD)
    {
        file->f_op = fops_info[minor_index].fops;
        if (file->f_op->open)
            return file->f_op->open(inode, file);
        else
            return 0;
    }
    else
    {
        printk(KERN_ERR "Is this even possible?\n");
        printk(KERN_ERR "Invalid minor %d\n", lamc_priv->minor);
        return -EINVAL;
    }
}


static struct file_operations base_fops = {
    .owner = THIS_MODULE,
    .open = amc525_lamc_pci_open,
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Device initialisation. */

/* In principle there may be multiple boards installed, so we'll allow for this
 * when allocating the device nodes. */

#define MAX_BOARDS          4
#define MAX_MINORS          (MAX_BOARDS * MINORS_PER_BOARD)

static struct class *device_class;  // Device class
static dev_t device_major;          // Major device number for our device
static long device_boards;          // Bit mask of allocated boards


/* Searches for an unallocated board number. */
static int get_free_board(unsigned int *board)
{
    for (int bit = 0; bit < MAX_BOARDS; bit ++)
    {
        if (test_and_set_bit(bit, &device_boards) == 0)
        {
            *board = bit;
            return 0;
        }
    }
    printk(KERN_ERR "Unable to allocate minor for device\n");
    return -EIO;
}

static void release_board(unsigned int board)
{
    test_and_clear_bit(board, &device_boards);
}


/* Performs basic PCI device initialisation. */
static int enable_board(struct pci_dev *pdev)
{
    int rc = pci_enable_device(pdev);
    TEST_RC(rc, no_device, "Unable to enable AMC525 LMBF\n");

    rc = pci_request_regions(pdev, DEVICE_NAME);
    TEST_RC(rc, no_regions, "Unable to reserve resources");

    rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
    TEST_RC(rc, no_dma_mask, "Unable to set DMA mask");

    pci_set_master(pdev);

    return 0;

no_dma_mask:
    pci_release_regions(pdev);
no_regions:
    pci_disable_device(pdev);
no_device:
    return rc;
}


static void disable_board(struct pci_dev *pdev)
{
    pci_clear_master(pdev);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
}


static int initialise_board(struct pci_dev *pdev, struct amc525_lamc_priv *lamc_priv)
{
    int rc = 0;
    pci_set_drvdata(pdev, lamc_priv);

    /* Map the register bar. */
    lamc_priv->reg_length = pci_resource_len(pdev, 0);
    lamc_priv->reg_memory = pci_iomap(pdev, 0, lamc_priv->reg_length);
    TEST_PTR(lamc_priv->reg_memory, rc, no_memory, "Unable to map bar");

    printk(KERN_INFO "Mapped bar: %ld %p\n",
        lamc_priv->reg_length, lamc_priv->reg_memory);
    return 0;

no_memory:
    return rc;
}


static void terminate_board(struct pci_dev *pdev)
{
    struct amc525_lamc_priv *lamc_priv = pci_get_drvdata(pdev);
    pci_iounmap(pdev, lamc_priv->reg_memory);
}


static int amc525_lamc_pci_probe(
    struct pci_dev *pdev, const struct pci_device_id *id)
{
    printk(KERN_INFO "Detected AMC525\n");
    int rc = 0;

    /* Ensure we can allocate a board number. */
    unsigned int board;
    rc = get_free_board(&board);
    TEST_RC(rc, no_board, "Unable to allocate board number\n");
    int major = MAJOR(device_major);
    int minor = board * MINORS_PER_BOARD;

    /* Allocate state for our board. */
    struct amc525_lamc_priv *lamc_priv = kmalloc(sizeof(struct amc525_lamc_priv), GFP_KERNEL);
    TEST_PTR(lamc_priv, rc, no_memory, "Unable to allocate memory");
    lamc_priv->dev = pdev;
    lamc_priv->board = board;
    lamc_priv->minor = minor;

    rc = enable_board(pdev);
    if (rc < 0)     goto no_enable;

    rc = initialise_board(pdev, lamc_priv);
    if (rc < 0)     goto no_initialise;

    cdev_init(&lamc_priv->cdev, &base_fops);
    lamc_priv->cdev.owner = THIS_MODULE;
    rc = cdev_add(&lamc_priv->cdev, MKDEV(major, minor), MINORS_PER_BOARD);
    TEST_RC(rc, no_cdev, "Unable to add device");

    for (int i = 0; i < MINORS_PER_BOARD; i ++)
        device_create(
            device_class, &pdev->dev, MKDEV(major, minor + i), NULL,
            "%s.%d.%s", DEVICE_NAME, board, fops_info[i].name);

    return 0;


    cdev_del(&lamc_priv->cdev);
no_cdev:
    terminate_board(pdev);
no_initialise:
    disable_board(pdev);
no_enable:
    kfree(lamc_priv);
no_memory:
    release_board(board);
no_board:
    return rc;
}


static void amc525_lamc_pci_remove(struct pci_dev *pdev)
{
    printk(KERN_INFO "Removing AMC525 device\n");
    struct amc525_lamc_priv *lamc_priv = pci_get_drvdata(pdev);
    int major = MAJOR(device_major);

    for (int i = 0; i < MINORS_PER_BOARD; i ++)
        device_destroy(device_class, MKDEV(major, lamc_priv->minor + i));
    cdev_del(&lamc_priv->cdev);
    terminate_board(pdev);
    disable_board(pdev);
    kfree(lamc_priv);
    release_board(lamc_priv->board);
}


static struct pci_driver amc525_lamc_pci_driver = {
    .name = DEVICE_NAME,
    .id_table = (const struct pci_device_id[]) {
        { PCI_DEVICE(XILINX_VID, AMC525_DID) },
    },
    .probe = amc525_lamc_pci_probe,
    .remove = amc525_lamc_pci_remove,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Driver initialisation. */


static int __init amc525_lamc_pci_init(void)
{
    printk(KERN_INFO "Loading AMC525 Lmodule\n");
    int rc = 0;

    /* Allocate major device number and create class. */
    rc = alloc_chrdev_region(&device_major, 0, MAX_MINORS, DEVICE_NAME);
    TEST_RC(rc, no_chrdev, "Unable to allocate dev region");

    device_class = class_create(THIS_MODULE, DEVICE_NAME);
    TEST_PTR(device_class, rc, no_class, "Unable to create class");

    rc = pci_register_driver(&amc525_lamc_pci_driver);
    TEST_RC(rc, no_driver, "Unable to register driver\n");
    printk(KERN_INFO "Registered AMC525 Ldriver\n");
    return rc;

no_driver:
    class_destroy(device_class);
no_class:
    unregister_chrdev_region(device_major, MAX_MINORS);
no_chrdev:
    return rc;
}


static void __exit amc525_lamc_pci_exit(void)
{
    printk(KERN_INFO "Unloading AMC525 Lmodule\n");
    pci_unregister_driver(&amc525_lamc_pci_driver);
    class_destroy(device_class);
    unregister_chrdev_region(device_major, MAX_MINORS);
}

module_init(amc525_lamc_pci_init);
module_exit(amc525_lamc_pci_exit);
