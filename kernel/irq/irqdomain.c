// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt)  "irq: " fmt

#include <linux/acpi.h>
#include <linux/debugfs.h>
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/topology.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/fs.h>

static LIST_HEAD(irq_domain_list);
static DEFINE_MUTEX(irq_domain_mutex);

static struct irq_domain *irq_default_domain;

static void irq_domain_check_hierarchy(struct irq_domain *domain);

struct irqchip_fwid {
	struct fwnode_handle	fwnode;
	unsigned int		type;
	char			*name;
	phys_addr_t		*pa;
};

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
static void debugfs_add_domain_dir(struct irq_domain *d);
static void debugfs_remove_domain_dir(struct irq_domain *d);
#else
static inline void debugfs_add_domain_dir(struct irq_domain *d) { }
static inline void debugfs_remove_domain_dir(struct irq_domain *d) { }
#endif

static const char *irqchip_fwnode_get_name(const struct fwnode_handle *fwnode)
{
	struct irqchip_fwid *fwid = container_of(fwnode, struct irqchip_fwid, fwnode);

	return fwid->name;
}

const struct fwnode_operations irqchip_fwnode_ops = {
	.get_name = irqchip_fwnode_get_name,
};
EXPORT_SYMBOL_GPL(irqchip_fwnode_ops);

/**
 * __irq_domain_alloc_fwnode - Allocate a fwnode_handle suitable for
 *                           identifying an irq domain
 * @type:	Type of irqchip_fwnode. See linux/irqdomain.h
 * @id:		Optional user provided id if name != NULL
 * @name:	Optional user provided domain name
 * @pa:		Optional user-provided physical address
 *
 * Allocate a struct irqchip_fwid, and return a pointer to the embedded
 * fwnode_handle (or NULL on failure).
 *
 * Note: The types IRQCHIP_FWNODE_NAMED and IRQCHIP_FWNODE_NAMED_ID are
 * solely to transport name information to irqdomain creation code. The
 * node is not stored. For other types the pointer is kept in the irq
 * domain struct.
 */
struct fwnode_handle *__irq_domain_alloc_fwnode(unsigned int type, int id,
						const char *name,
						phys_addr_t *pa)
{
	struct irqchip_fwid *fwid;
	char *n;

	fwid = kzalloc(sizeof(*fwid), GFP_KERNEL);

	switch (type) {
	case IRQCHIP_FWNODE_NAMED:
		n = kasprintf(GFP_KERNEL, "%s", name);
		break;
	case IRQCHIP_FWNODE_NAMED_ID:
		n = kasprintf(GFP_KERNEL, "%s-%d", name, id);
		break;
	default:
		n = kasprintf(GFP_KERNEL, "irqchip@%pa", pa);
		break;
	}

	if (!fwid || !n) {
		kfree(fwid);
		kfree(n);
		return NULL;
	}

	fwid->type = type;
	fwid->name = n;
	fwid->pa = pa;
	fwnode_init(&fwid->fwnode, &irqchip_fwnode_ops);
	return &fwid->fwnode;
}
EXPORT_SYMBOL_GPL(__irq_domain_alloc_fwnode);

/**
 * irq_domain_free_fwnode - Free a non-OF-backed fwnode_handle
 *
 * Free a fwnode_handle allocated with irq_domain_alloc_fwnode.
 */
void irq_domain_free_fwnode(struct fwnode_handle *fwnode)
{
	struct irqchip_fwid *fwid;

	if (WARN_ON(!is_fwnode_irqchip(fwnode)))
		return;

	fwid = container_of(fwnode, struct irqchip_fwid, fwnode);
	kfree(fwid->name);
	kfree(fwid);
}
EXPORT_SYMBOL_GPL(irq_domain_free_fwnode);

/**
 * __irq_domain_add() - Allocate a new irq_domain data structure
 * @fwnode: firmware node for the interrupt controller
 * @size: Size of linear map; 0 for radix mapping only
 * @hwirq_max: Maximum number of interrupts supported by controller
 * @direct_max: Maximum value of direct maps; Use ~0 for no limit; 0 for no
 *              direct mapping
 * @ops: domain callbacks
 * @host_data: Controller private data pointer
 *
 * Allocates and initializes an irq_domain structure.
 * Returns pointer to IRQ domain, or NULL on failure.
 */
/*
 * IAMROOT, 2022.10.01:
 * - legacy
 *   legacy irq_domain_create_legacy
 * - simple
 *   irq_domain_create_simple
 * - linear
 *   irq_domain_create_linear
 * - nomap
 *   irq_domain_add_nomap
 * - tree
 *   irq_domain_create_tree
 * 
 *          size                hwirq_max          direct_max (mapping)
 * nomap  | 0                   max_irq            max_irq
 * linear | size                size               0
 * tree   | 0                   ~0                 0
 * simple | size                size               0           yes
 * legacy | size + first_hwirq  size + first_hwirq 0           yes
 *
 * - ops example : gic_irq_domain_ops
 */
struct irq_domain *__irq_domain_add(struct fwnode_handle *fwnode, unsigned int size,
				    irq_hw_number_t hwirq_max, int direct_max,
				    const struct irq_domain_ops *ops,
				    void *host_data)
{
	struct irqchip_fwid *fwid;
	struct irq_domain *domain;

	static atomic_t unknown_domains;

/*
 * IAMROOT, 2022.10.01:
 * - size && direct_max 인 경우는 없다.
 * - nomap인 경우만 direct_max가 존재한다.
 */
	if (WARN_ON((size && direct_max) ||
		    (!IS_ENABLED(CONFIG_IRQ_DOMAIN_NOMAP) && direct_max)))
		return NULL;

/*
 * IAMROOT, 2022.10.01:
 * - linear mapping 배열을 size만큼 만든다.
 *   reverse map. struct irq_data __rcu
 *
 */
	domain = kzalloc_node(struct_size(domain, revmap, size),
			      GFP_KERNEL, of_node_to_nid(to_of_node(fwnode)));
	if (!domain)
		return NULL;

	if (is_fwnode_irqchip(fwnode)) {
/*
 * IAMROOT, 2022.10.01:
 * - dt에서 왔으면 type에 따라 name, fwnode, flags를 초기화한다.
 */
		fwid = container_of(fwnode, struct irqchip_fwid, fwnode);

		switch (fwid->type) {
		case IRQCHIP_FWNODE_NAMED:
		case IRQCHIP_FWNODE_NAMED_ID:
			domain->fwnode = fwnode;
			domain->name = kstrdup(fwid->name, GFP_KERNEL);
			if (!domain->name) {
				kfree(domain);
				return NULL;
			}
			domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;
			break;
/*
 * IAMROOT, 2022.10.01:
 * - IRQCHIP_FWNODE_REAL
 */
		default:
			domain->fwnode = fwnode;
			domain->name = fwid->name;
			break;
		}
	} else if (is_of_node(fwnode) || is_acpi_device_node(fwnode) ||
		   is_software_node(fwnode)) {

/*
 * IAMROOT, 2022.10.01:
 * - 이경우엔 이름을 %pfw형식으로 만들어서 넣는다.
 */
		char *name;

		/*
		 * fwnode paths contain '/', which debugfs is legitimately
		 * unhappy about. Replace them with ':', which does
		 * the trick and is not as offensive as '\'...
		 */
		name = kasprintf(GFP_KERNEL, "%pfw", fwnode);
		if (!name) {
			kfree(domain);
			return NULL;
		}

		strreplace(name, '/', ':');

		domain->name = name;
		domain->fwnode = fwnode;
		domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;
	}

/*
 * IAMROOT, 2022.10.01:
 * - 이렇게 했는데도 이름이 없으면 unknown으로 만든다.
 */
	if (!domain->name) {
		if (fwnode)
			pr_err("Invalid fwnode type for irqdomain\n");
		domain->name = kasprintf(GFP_KERNEL, "unknown-%d",
					 atomic_inc_return(&unknown_domains));
		if (!domain->name) {
			kfree(domain);
			return NULL;
		}
		domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;
	}

	fwnode_handle_get(fwnode);
	fwnode_dev_initialized(fwnode, true);

	/* Fill structure */
	INIT_RADIX_TREE(&domain->revmap_tree, GFP_KERNEL);
	mutex_init(&domain->revmap_mutex);
	domain->ops = ops;
	domain->host_data = host_data;
	domain->hwirq_max = hwirq_max;

/*
 * IAMROOT, 2022.10.01:
 * - direct_max는 nomap만 해당된다.
 */
	if (direct_max) {
		size = direct_max;
		domain->flags |= IRQ_DOMAIN_FLAG_NO_MAP;
	}

	domain->revmap_size = size;

	irq_domain_check_hierarchy(domain);

	mutex_lock(&irq_domain_mutex);
	debugfs_add_domain_dir(domain);
	list_add(&domain->link, &irq_domain_list);
	mutex_unlock(&irq_domain_mutex);

	pr_debug("Added domain %s\n", domain->name);
	return domain;
}
EXPORT_SYMBOL_GPL(__irq_domain_add);

/**
 * irq_domain_remove() - Remove an irq domain.
 * @domain: domain to remove
 *
 * This routine is used to remove an irq domain. The caller must ensure
 * that all mappings within the domain have been disposed of prior to
 * use, depending on the revmap type.
 */
void irq_domain_remove(struct irq_domain *domain)
{
	mutex_lock(&irq_domain_mutex);
	debugfs_remove_domain_dir(domain);

	WARN_ON(!radix_tree_empty(&domain->revmap_tree));

	list_del(&domain->link);

	/*
	 * If the going away domain is the default one, reset it.
	 */
	if (unlikely(irq_default_domain == domain))
		irq_set_default_host(NULL);

	mutex_unlock(&irq_domain_mutex);

	pr_debug("Removed domain %s\n", domain->name);

	fwnode_dev_initialized(domain->fwnode, false);
	fwnode_handle_put(domain->fwnode);
	if (domain->flags & IRQ_DOMAIN_NAME_ALLOCATED)
		kfree(domain->name);
	kfree(domain);
}
EXPORT_SYMBOL_GPL(irq_domain_remove);

/*
 * IAMROOT, 2022.10.08:
 * - @bus_token으로 @domain의 bus_token을 설정하고 debugfs에 등록한다.
 */
void irq_domain_update_bus_token(struct irq_domain *domain,
				 enum irq_domain_bus_token bus_token)
{
	char *name;

/*
 * IAMROOT, 2022.10.08:
 * - 이미 @bus_token으로 설정되있다면 return.
 */
	if (domain->bus_token == bus_token)
		return;

	mutex_lock(&irq_domain_mutex);

	domain->bus_token = bus_token;

	name = kasprintf(GFP_KERNEL, "%s-%d", domain->name, bus_token);
	if (!name) {
		mutex_unlock(&irq_domain_mutex);
		return;
	}

	debugfs_remove_domain_dir(domain);

	if (domain->flags & IRQ_DOMAIN_NAME_ALLOCATED)
		kfree(domain->name);
	else
		domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;

	domain->name = name;
	debugfs_add_domain_dir(domain);

	mutex_unlock(&irq_domain_mutex);
}
EXPORT_SYMBOL_GPL(irq_domain_update_bus_token);

/**
 * irq_domain_create_simple() - Register an irq_domain and optionally map a range of irqs
 * @fwnode: firmware node for the interrupt controller
 * @size: total number of irqs in mapping
 * @first_irq: first number of irq block assigned to the domain,
 *	pass zero to assign irqs on-the-fly. If first_irq is non-zero, then
 *	pre-map all of the irqs in the domain to virqs starting at first_irq.
 * @ops: domain callbacks
 * @host_data: Controller private data pointer
 *
 * Allocates an irq_domain, and optionally if first_irq is positive then also
 * allocate irq_descs and map all of the hwirqs to virqs starting at first_irq.
 *
 * This is intended to implement the expected behaviour for most
 * interrupt controllers. If device tree is used, then first_irq will be 0 and
 * irqs get mapped dynamically on the fly. However, if the controller requires
 * static virq assignments (non-DT boot) then it will set that up correctly.
 */
struct irq_domain *irq_domain_create_simple(struct fwnode_handle *fwnode,
					    unsigned int size,
					    unsigned int first_irq,
					    const struct irq_domain_ops *ops,
					    void *host_data)
{
	struct irq_domain *domain;

	domain = __irq_domain_add(fwnode, size, size, 0, ops, host_data);
	if (!domain)
		return NULL;

	if (first_irq > 0) {
		if (IS_ENABLED(CONFIG_SPARSE_IRQ)) {
			/* attempt to allocated irq_descs */
			int rc = irq_alloc_descs(first_irq, first_irq, size,
						 of_node_to_nid(to_of_node(fwnode)));
			if (rc < 0)
				pr_info("Cannot allocate irq_descs @ IRQ%d, assuming pre-allocated\n",
					first_irq);
		}
		irq_domain_associate_many(domain, first_irq, 0, size);
	}

	return domain;
}
EXPORT_SYMBOL_GPL(irq_domain_create_simple);

/**
 * irq_domain_add_legacy() - Allocate and register a legacy revmap irq_domain.
 * @of_node: pointer to interrupt controller's device tree node.
 * @size: total number of irqs in legacy mapping
 * @first_irq: first number of irq block assigned to the domain
 * @first_hwirq: first hwirq number to use for the translation. Should normally
 *               be '0', but a positive integer can be used if the effective
 *               hwirqs numbering does not begin at zero.
 * @ops: map/unmap domain callbacks
 * @host_data: Controller private data pointer
 *
 * Note: the map() callback will be called before this function returns
 * for all legacy interrupts except 0 (which is always the invalid irq for
 * a legacy controller).
 */
struct irq_domain *irq_domain_add_legacy(struct device_node *of_node,
					 unsigned int size,
					 unsigned int first_irq,
					 irq_hw_number_t first_hwirq,
					 const struct irq_domain_ops *ops,
					 void *host_data)
{
	return irq_domain_create_legacy(of_node_to_fwnode(of_node), size,
					first_irq, first_hwirq, ops, host_data);
}
EXPORT_SYMBOL_GPL(irq_domain_add_legacy);

struct irq_domain *irq_domain_create_legacy(struct fwnode_handle *fwnode,
					 unsigned int size,
					 unsigned int first_irq,
					 irq_hw_number_t first_hwirq,
					 const struct irq_domain_ops *ops,
					 void *host_data)
{
	struct irq_domain *domain;

	domain = __irq_domain_add(fwnode, first_hwirq + size, first_hwirq + size, 0, ops, host_data);
	if (domain)
		irq_domain_associate_many(domain, first_irq, first_hwirq, size);

	return domain;
}
EXPORT_SYMBOL_GPL(irq_domain_create_legacy);

/**
 * irq_find_matching_fwspec() - Locates a domain for a given fwspec
 * @fwspec: FW specifier for an interrupt
 * @bus_token: domain-specific data
 */
/*
 * IAMROOT, 2022.10.29:
 * - fwspec과 매칭되는 irq_domain을 irq_domain_list에서 찾는다.
 */
struct irq_domain *irq_find_matching_fwspec(struct irq_fwspec *fwspec,
					    enum irq_domain_bus_token bus_token)
{
	struct irq_domain *h, *found = NULL;
	struct fwnode_handle *fwnode = fwspec->fwnode;
	int rc;

	/* We might want to match the legacy controller last since
	 * it might potentially be set to match all interrupts in
	 * the absence of a device node. This isn't a problem so far
	 * yet though...
	 *
	 * bus_token == DOMAIN_BUS_ANY matches any domain, any other
	 * values must generate an exact match for the domain to be
	 * selected.
	 */
	mutex_lock(&irq_domain_mutex);

/*
 * IAMROOT, 2022.10.29:
 * - ex) gic_irq_domain_ops에서
 *      select의 경우 : gic_irq_domain_select
 * fwspec과 매칭되는 irq_domain을 irq_domain_list에서 찾는다.
 */
	list_for_each_entry(h, &irq_domain_list, link) {
		if (h->ops->select && fwspec->param_count)
			rc = h->ops->select(h, fwspec, bus_token);
		else if (h->ops->match)
			rc = h->ops->match(h, to_of_node(fwnode), bus_token);
		else
			rc = ((fwnode != NULL) && (h->fwnode == fwnode) &&
			      ((bus_token == DOMAIN_BUS_ANY) ||
			       (h->bus_token == bus_token)));

		if (rc) {
			found = h;
			break;
		}
	}
	mutex_unlock(&irq_domain_mutex);
	return found;
}
EXPORT_SYMBOL_GPL(irq_find_matching_fwspec);

/**
 * irq_domain_check_msi_remap - Check whether all MSI irq domains implement
 * IRQ remapping
 *
 * Return: false if any MSI irq domain does not support IRQ remapping,
 * true otherwise (including if there is no MSI irq domain)
 */
bool irq_domain_check_msi_remap(void)
{
	struct irq_domain *h;
	bool ret = true;

	mutex_lock(&irq_domain_mutex);
	list_for_each_entry(h, &irq_domain_list, link) {
		if (irq_domain_is_msi(h) &&
		    !irq_domain_hierarchical_is_msi_remap(h)) {
			ret = false;
			break;
		}
	}
	mutex_unlock(&irq_domain_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(irq_domain_check_msi_remap);

/**
 * irq_set_default_host() - Set a "default" irq domain
 * @domain: default domain pointer
 *
 * For convenience, it's possible to set a "default" domain that will be used
 * whenever NULL is passed to irq_create_mapping(). It makes life easier for
 * platforms that want to manipulate a few hard coded interrupt numbers that
 * aren't properly represented in the device-tree.
 */
void irq_set_default_host(struct irq_domain *domain)
{
	pr_debug("Default domain set to @0x%p\n", domain);

	irq_default_domain = domain;
}
EXPORT_SYMBOL_GPL(irq_set_default_host);

/**
 * irq_get_default_host() - Retrieve the "default" irq domain
 *
 * Returns: the default domain, if any.
 *
 * Modern code should never use this. This should only be used on
 * systems that cannot implement a firmware->fwnode mapping (which
 * both DT and ACPI provide).
 */
struct irq_domain *irq_get_default_host(void)
{
	return irq_default_domain;
}
EXPORT_SYMBOL_GPL(irq_get_default_host);

static bool irq_domain_is_nomap(struct irq_domain *domain)
{
	return IS_ENABLED(CONFIG_IRQ_DOMAIN_NOMAP) &&
	       (domain->flags & IRQ_DOMAIN_FLAG_NO_MAP);
}

static void irq_domain_clear_mapping(struct irq_domain *domain,
				     irq_hw_number_t hwirq)
{
	if (irq_domain_is_nomap(domain))
		return;

	mutex_lock(&domain->revmap_mutex);
	if (hwirq < domain->revmap_size)
		rcu_assign_pointer(domain->revmap[hwirq], NULL);
	else
		radix_tree_delete(&domain->revmap_tree, hwirq);
	mutex_unlock(&domain->revmap_mutex);
}

/*
 * IAMROOT, 2022.10.15:
 * - @domain에 hwirq번호를 key로 irq_data를 mapping한다.
 */
static void irq_domain_set_mapping(struct irq_domain *domain,
				   irq_hw_number_t hwirq,
				   struct irq_data *irq_data)
{
	if (irq_domain_is_nomap(domain))
		return;

	mutex_lock(&domain->revmap_mutex);
	if (hwirq < domain->revmap_size)
		rcu_assign_pointer(domain->revmap[hwirq], irq_data);
	else
		radix_tree_insert(&domain->revmap_tree, hwirq, irq_data);
	mutex_unlock(&domain->revmap_mutex);
}

static void irq_domain_disassociate(struct irq_domain *domain, unsigned int irq)
{
	struct irq_data *irq_data = irq_get_irq_data(irq);
	irq_hw_number_t hwirq;

	if (WARN(!irq_data || irq_data->domain != domain,
		 "virq%i doesn't exist; cannot disassociate\n", irq))
		return;

	hwirq = irq_data->hwirq;
	irq_set_status_flags(irq, IRQ_NOREQUEST);

	/* remove chip and handler */
	irq_set_chip_and_handler(irq, NULL, NULL);

	/* Make sure it's completed */
	synchronize_irq(irq);

	/* Tell the PIC about it */
	if (domain->ops->unmap)
		domain->ops->unmap(domain, irq);
	smp_mb();

	irq_data->domain = NULL;
	irq_data->hwirq = 0;
	domain->mapcount--;

	/* Clear reverse map for this hwirq */
	irq_domain_clear_mapping(domain, hwirq);
}

/*
 * IAMROOT, 2022.10.29:
 * - @virq에 @domain, hwirq를 mapping한다.
 *   map ops가 있으면 실행을 해준다. @domain 자료구조에는 무조건  넣어준다.
 */
int irq_domain_associate(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);
	int ret;

	if (WARN(hwirq >= domain->hwirq_max,
		 "error: hwirq 0x%x is too large for %s\n", (int)hwirq, domain->name))
		return -EINVAL;
	if (WARN(!irq_data, "error: virq%i is not allocated", virq))
		return -EINVAL;
	if (WARN(irq_data->domain, "error: virq%i is already associated", virq))
		return -EINVAL;

	mutex_lock(&irq_domain_mutex);
	irq_data->hwirq = hwirq;
	irq_data->domain = domain;
/*
 * IAMROOT, 2022.10.29:
 * - gic는 map ops가 없다.
 */
	if (domain->ops->map) {
		ret = domain->ops->map(domain, virq, hwirq);
		if (ret != 0) {
			/*
			 * If map() returns -EPERM, this interrupt is protected
			 * by the firmware or some other service and shall not
			 * be mapped. Don't bother telling the user about it.
			 */
			if (ret != -EPERM) {
				pr_info("%s didn't like hwirq-0x%lx to VIRQ%i mapping (rc=%d)\n",
				       domain->name, hwirq, virq, ret);
			}
			irq_data->domain = NULL;
			irq_data->hwirq = 0;
			mutex_unlock(&irq_domain_mutex);
			return ret;
		}

		/* If not already assigned, give the domain the chip's name */
		if (!domain->name && irq_data->chip)
			domain->name = irq_data->chip->name;
	}

	domain->mapcount++;
	irq_domain_set_mapping(domain, hwirq, irq_data);
	mutex_unlock(&irq_domain_mutex);

	irq_clear_status_flags(virq, IRQ_NOREQUEST);

	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_associate);

void irq_domain_associate_many(struct irq_domain *domain, unsigned int irq_base,
			       irq_hw_number_t hwirq_base, int count)
{
	struct device_node *of_node;
	int i;

	of_node = irq_domain_get_of_node(domain);
	pr_debug("%s(%s, irqbase=%i, hwbase=%i, count=%i)\n", __func__,
		of_node_full_name(of_node), irq_base, (int)hwirq_base, count);

	for (i = 0; i < count; i++) {
		irq_domain_associate(domain, irq_base + i, hwirq_base + i);
	}
}
EXPORT_SYMBOL_GPL(irq_domain_associate_many);

#ifdef CONFIG_IRQ_DOMAIN_NOMAP
/**
 * irq_create_direct_mapping() - Allocate an irq for direct mapping
 * @domain: domain to allocate the irq for or NULL for default domain
 *
 * This routine is used for irq controllers which can choose the hardware
 * interrupt numbers they generate. In such a case it's simplest to use
 * the linux irq as the hardware interrupt number. It still uses the linear
 * or radix tree to store the mapping, but the irq controller can optimize
 * the revmap path by using the hwirq directly.
 */
unsigned int irq_create_direct_mapping(struct irq_domain *domain)
{
	struct device_node *of_node;
	unsigned int virq;

	if (domain == NULL)
		domain = irq_default_domain;

	of_node = irq_domain_get_of_node(domain);
	virq = irq_alloc_desc_from(1, of_node_to_nid(of_node));
	if (!virq) {
		pr_debug("create_direct virq allocation failed\n");
		return 0;
	}
	if (virq >= domain->revmap_size) {
		pr_err("ERROR: no free irqs available below %i maximum\n",
			domain->revmap_size);
		irq_free_desc(virq);
		return 0;
	}
	pr_debug("create_direct obtained virq %d\n", virq);

	if (irq_domain_associate(domain, virq, virq)) {
		irq_free_desc(virq);
		return 0;
	}

	return virq;
}
EXPORT_SYMBOL_GPL(irq_create_direct_mapping);
#endif

/**
 * irq_create_mapping_affinity() - Map a hardware interrupt into linux irq space
 * @domain: domain owning this hardware interrupt or NULL for default domain
 * @hwirq: hardware irq number in that domain space
 * @affinity: irq affinity
 *
 * Only one mapping per hardware interrupt is permitted. Returns a linux
 * irq number.
 * If the sense/trigger is to be specified, set_irq_type() should be called
 * on the number returned from that call.
 */
/*
 * IAMROOT, 2022.10.29:
 * - @hwirq의 virq를 @domain에서 얻어온다. 없을경우 생성 및 mapping을 한다.
 */
unsigned int irq_create_mapping_affinity(struct irq_domain *domain,
				       irq_hw_number_t hwirq,
				       const struct irq_affinity_desc *affinity)
{
	struct device_node *of_node;
	int virq;

	pr_debug("irq_create_mapping(0x%p, 0x%lx)\n", domain, hwirq);

	/* Look for default domain if necessary */
	if (domain == NULL)
		domain = irq_default_domain;
	if (domain == NULL) {
		WARN(1, "%s(, %lx) called with NULL domain\n", __func__, hwirq);
		return 0;
	}
	pr_debug("-> using domain @%p\n", domain);

	of_node = irq_domain_get_of_node(domain);

	/* Check if mapping already exists */
/*
 * IAMROOT, 2022.10.29:
 * - @domain에서 hwirq를 구해봐서 virq가 있으면 return. 없으면 생성후 @domain에
 *   등록, mapping하여 return 한다.
 */
	virq = irq_find_mapping(domain, hwirq);
	if (virq) {
		pr_debug("-> existing mapping on virq %d\n", virq);
		return virq;
	}

	/* Allocate a virtual interrupt number */
	virq = irq_domain_alloc_descs(-1, 1, hwirq, of_node_to_nid(of_node),
				      affinity);
	if (virq <= 0) {
		pr_debug("-> virq allocation failed\n");
		return 0;
	}

	if (irq_domain_associate(domain, virq, hwirq)) {
		irq_free_desc(virq);
		return 0;
	}

	pr_debug("irq %lu on domain %s mapped to virtual irq %u\n",
		hwirq, of_node_full_name(of_node), virq);

	return virq;
}
EXPORT_SYMBOL_GPL(irq_create_mapping_affinity);

static int irq_domain_translate(struct irq_domain *d,
				struct irq_fwspec *fwspec,
				irq_hw_number_t *hwirq, unsigned int *type)
{
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
	if (d->ops->translate)
		return d->ops->translate(d, fwspec, hwirq, type);
#endif
	if (d->ops->xlate)
		return d->ops->xlate(d, to_of_node(fwspec->fwnode),
				     fwspec->param, fwspec->param_count,
				     hwirq, type);

	/* If domain has no translation, then we assume interrupt line */
	*hwirq = fwspec->param[0];
	return 0;
}

static void of_phandle_args_to_fwspec(struct device_node *np, const u32 *args,
				      unsigned int count,
				      struct irq_fwspec *fwspec)
{
	int i;

	fwspec->fwnode = of_node_to_fwnode(np);
	fwspec->param_count = count;

	for (i = 0; i < count; i++)
		fwspec->param[i] = args[i];
}


/*
 * IAMROOT, 2022.10.29:
 * - @fwspec의 hwirq로 virq를 등록 및 mapping한다.
 */
unsigned int irq_create_fwspec_mapping(struct irq_fwspec *fwspec)
{
	struct irq_domain *domain;
	struct irq_data *irq_data;
	irq_hw_number_t hwirq;
	unsigned int type = IRQ_TYPE_NONE;
	int virq;

	if (fwspec->fwnode) {
/*
 * IAMROOT, 2022.10.29:
 * - 처음에 가장 많이 쓰는 wired로 먼저 검색해보고 아니면 any로 검사한다.
 */
		domain = irq_find_matching_fwspec(fwspec, DOMAIN_BUS_WIRED);
		if (!domain)
			domain = irq_find_matching_fwspec(fwspec, DOMAIN_BUS_ANY);
	} else {
		domain = irq_default_domain;
	}

	if (!domain) {
		pr_warn("no irq domain found for %s !\n",
			of_node_full_name(to_of_node(fwspec->fwnode)));
		return 0;
	}

/*
 * IAMROOT, 2022.10.29:
 * - 찾아온 domain으로 hwirq, type을 구해온다.
 */
	if (irq_domain_translate(domain, fwspec, &hwirq, &type))
		return 0;

	/*
	 * WARN if the irqchip returns a type with bits
	 * outside the sense mask set and clear these bits.
	 */
/*
 * IAMROOT, 2022.10.29:
 * - IRQ_TYPE_SENSE_MASK외에 다른값들이 있으면 안된다. IRQ_TYPE_SENSE_MASK만 남긴다.
 */
	if (WARN_ON(type & ~IRQ_TYPE_SENSE_MASK))
		type &= IRQ_TYPE_SENSE_MASK;

	/*
	 * If we've already configured this interrupt,
	 * don't do it again, or hell will break loose.
	 */
	virq = irq_find_mapping(domain, hwirq);

/*
 * IAMROOT, 2022.10.29:
 * - hwirq에 해당하는 virq가 있으면 fwspec에서 정의된값과 일치한경우 즉시 return.
 */
	if (virq) {
		/*
		 * If the trigger type is not specified or matches the
		 * current trigger type then we are done so return the
		 * interrupt number.
		 */
/*
 * IAMROOT, 2022.10.29:
 * - fwspec에 type 지정이 안됬거나, virq와 fwspec type과 동일한 경우 return..
 */
		if (type == IRQ_TYPE_NONE || type == irq_get_trigger_type(virq))
			return virq;

		/*
		 * If the trigger type has not been set yet, then set
		 * it now and return the interrupt number.
		 */
/*
 * IAMROOT, 2022.10.29:
 * - virq에 아직 type설정이 안된경우 fwspec의 type으로 지정한다.
 */
		if (irq_get_trigger_type(virq) == IRQ_TYPE_NONE) {
			irq_data = irq_get_irq_data(virq);
			if (!irq_data)
				return 0;

			irqd_set_trigger_type(irq_data, type);
			return virq;
		}

/*
 * IAMROOT, 2022.10.29:
 * - 이미 virq에 설정되있는데 fwspec과 일치하지 않는다. 바꾸진 않고 warn후 return err.
 */
		pr_warn("type mismatch, failed to map hwirq-%lu for %s!\n",
			hwirq, of_node_full_name(to_of_node(fwspec->fwnode)));
		return 0;
	}

/*
 * IAMROOT, 2022.10.29:
 * - 아직 hwirq에 대한 virq가 없는상태. virq를 구해와야된다.
 */

/*
 * IAMROOT, 2022.10.29:
 * - @fwspec으로 @domain에 virq 1개를 아무노드에서 구해온다.
 */
	if (irq_domain_is_hierarchy(domain)) {
		virq = irq_domain_alloc_irqs(domain, 1, NUMA_NO_NODE, fwspec);
		if (virq <= 0)
			return 0;
	} else {
		/* Create mapping */
		virq = irq_create_mapping(domain, hwirq);
		if (!virq)
			return virq;
	}

	irq_data = irq_get_irq_data(virq);
/*
 * IAMROOT, 2022.10.29:
 * - error처리.
 */
	if (!irq_data) {
		if (irq_domain_is_hierarchy(domain))
			irq_domain_free_irqs(virq, 1);
		else
			irq_dispose_mapping(virq);
		return 0;
	}

/*
 * IAMROOT, 2022.10.29:
 * - @type을 irq_data에 설정한다.
 */
	/* Store trigger type */
	irqd_set_trigger_type(irq_data, type);

	return virq;
}
EXPORT_SYMBOL_GPL(irq_create_fwspec_mapping);

unsigned int irq_create_of_mapping(struct of_phandle_args *irq_data)
{
	struct irq_fwspec fwspec;

	of_phandle_args_to_fwspec(irq_data->np, irq_data->args,
				  irq_data->args_count, &fwspec);

	return irq_create_fwspec_mapping(&fwspec);
}
EXPORT_SYMBOL_GPL(irq_create_of_mapping);

/**
 * irq_dispose_mapping() - Unmap an interrupt
 * @virq: linux irq number of the interrupt to unmap
 */
void irq_dispose_mapping(unsigned int virq)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);
	struct irq_domain *domain;

	if (!virq || !irq_data)
		return;

	domain = irq_data->domain;
	if (WARN_ON(domain == NULL))
		return;

	if (irq_domain_is_hierarchy(domain)) {
		irq_domain_free_irqs(virq, 1);
	} else {
		irq_domain_disassociate(domain, virq);
		irq_free_desc(virq);
	}
}
EXPORT_SYMBOL_GPL(irq_dispose_mapping);

/**
 * __irq_resolve_mapping() - Find a linux irq from a hw irq number.
 * @domain: domain owning this hardware interrupt
 * @hwirq: hardware irq number in that domain space
 * @irq: optional pointer to return the Linux irq if required
 *
 * Returns the interrupt descriptor.
 */
/*
 * IAMROOT, 2022.10.29:
 * - nomap, linear, tree인지에 따라 검색해온다.
 */
struct irq_desc *__irq_resolve_mapping(struct irq_domain *domain,
				       irq_hw_number_t hwirq,
				       unsigned int *irq)
{
	struct irq_desc *desc = NULL;
	struct irq_data *data;

	/* Look for default domain if necessary */
	if (domain == NULL)
		domain = irq_default_domain;
	if (domain == NULL)
		return desc;

/*
 * IAMROOT, 2022.10.29:
 * - nomap
 */
	if (irq_domain_is_nomap(domain)) {
		if (hwirq < domain->revmap_size) {
			data = irq_domain_get_irq_data(domain, hwirq);
			if (data && data->hwirq == hwirq)
				desc = irq_data_to_desc(data);
		}

		return desc;
	}

/*
 * IAMROOT, 2022.10.29:
 * - linear & tree
 */
	rcu_read_lock();
	/* Check if the hwirq is in the linear revmap. */
	if (hwirq < domain->revmap_size)
		data = rcu_dereference(domain->revmap[hwirq]);
	else
		data = radix_tree_lookup(&domain->revmap_tree, hwirq);

	if (likely(data)) {
		desc = irq_data_to_desc(data);
		if (irq)
			*irq = data->irq;
	}

	rcu_read_unlock();
	return desc;
}
EXPORT_SYMBOL_GPL(__irq_resolve_mapping);

/**
 * irq_domain_xlate_onecell() - Generic xlate for direct one cell bindings
 *
 * Device Tree IRQ specifier translation function which works with one cell
 * bindings where the cell value maps directly to the hwirq number.
 */
int irq_domain_xlate_onecell(struct irq_domain *d, struct device_node *ctrlr,
			     const u32 *intspec, unsigned int intsize,
			     unsigned long *out_hwirq, unsigned int *out_type)
{
	if (WARN_ON(intsize < 1))
		return -EINVAL;
	*out_hwirq = intspec[0];
	*out_type = IRQ_TYPE_NONE;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_onecell);

/**
 * irq_domain_xlate_twocell() - Generic xlate for direct two cell bindings
 *
 * Device Tree IRQ specifier translation function which works with two cell
 * bindings where the cell values map directly to the hwirq number
 * and linux irq flags.
 */
int irq_domain_xlate_twocell(struct irq_domain *d, struct device_node *ctrlr,
			const u32 *intspec, unsigned int intsize,
			irq_hw_number_t *out_hwirq, unsigned int *out_type)
{
	struct irq_fwspec fwspec;

	of_phandle_args_to_fwspec(ctrlr, intspec, intsize, &fwspec);
	return irq_domain_translate_twocell(d, &fwspec, out_hwirq, out_type);
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_twocell);

/**
 * irq_domain_xlate_onetwocell() - Generic xlate for one or two cell bindings
 *
 * Device Tree IRQ specifier translation function which works with either one
 * or two cell bindings where the cell values map directly to the hwirq number
 * and linux irq flags.
 *
 * Note: don't use this function unless your interrupt controller explicitly
 * supports both one and two cell bindings.  For the majority of controllers
 * the _onecell() or _twocell() variants above should be used.
 */
int irq_domain_xlate_onetwocell(struct irq_domain *d,
				struct device_node *ctrlr,
				const u32 *intspec, unsigned int intsize,
				unsigned long *out_hwirq, unsigned int *out_type)
{
	if (WARN_ON(intsize < 1))
		return -EINVAL;
	*out_hwirq = intspec[0];
	if (intsize > 1)
		*out_type = intspec[1] & IRQ_TYPE_SENSE_MASK;
	else
		*out_type = IRQ_TYPE_NONE;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_onetwocell);

const struct irq_domain_ops irq_domain_simple_ops = {
	.xlate = irq_domain_xlate_onetwocell,
};
EXPORT_SYMBOL_GPL(irq_domain_simple_ops);

/**
 * irq_domain_translate_onecell() - Generic translate for direct one cell
 * bindings
 */
int irq_domain_translate_onecell(struct irq_domain *d,
				 struct irq_fwspec *fwspec,
				 unsigned long *out_hwirq,
				 unsigned int *out_type)
{
	if (WARN_ON(fwspec->param_count < 1))
		return -EINVAL;
	*out_hwirq = fwspec->param[0];
	*out_type = IRQ_TYPE_NONE;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_translate_onecell);

/**
 * irq_domain_translate_twocell() - Generic translate for direct two cell
 * bindings
 *
 * Device Tree IRQ specifier translation function which works with two cell
 * bindings where the cell values map directly to the hwirq number
 * and linux irq flags.
 */
int irq_domain_translate_twocell(struct irq_domain *d,
				 struct irq_fwspec *fwspec,
				 unsigned long *out_hwirq,
				 unsigned int *out_type)
{
	if (WARN_ON(fwspec->param_count < 2))
		return -EINVAL;
	*out_hwirq = fwspec->param[0];
	*out_type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_translate_twocell);

/*
 * IAMROOT, 2022.10.15:
 * - @virq 번호가 있는경우 @virq부터 @cnt만큼 할당시도를 한다,.
 * - 그게 아니라면 hwirq로 virq start번호를 만들어 시도를 한다.
 */
int irq_domain_alloc_descs(int virq, unsigned int cnt, irq_hw_number_t hwirq,
			   int node, const struct irq_affinity_desc *affinity)
{
	unsigned int hint;

	if (virq >= 0) {
/*
 * IAMROOT, 2022.10.15:
 * - 특정 번호의 virq요청이있으면, 해당 번호로 시작해서, cnt만큼 구해오는걸
 *   시도한다.
 */
		virq = __irq_alloc_descs(virq, virq, cnt, node, THIS_MODULE,
					 affinity);
	} else {
/*
 * IAMROOT, 2022.10.15:
 * - 시작번호를 선정한다. 가능하면 virq번호를 hwirq와 맞추고 싶어한다.
 */
		hint = hwirq % nr_irqs;

/*
 * IAMROOT, 2022.10.15:
 * - virq는 0번부터 시작할수없다. 예외처리.
 */
		if (hint == 0)
			hint++;
		virq = __irq_alloc_descs(-1, hint, cnt, node, THIS_MODULE,
					 affinity);

/*
 * IAMROOT, 2022.10.15:
 * - hint ~ hint + cnt - 1에서 실패를 했다면, 1번부터로 범위를 바꿔 다시 한번
 *   시도한다.
 */
		if (virq <= 0 && hint > 1) {
			virq = __irq_alloc_descs(-1, 1, cnt, node, THIS_MODULE,
						 affinity);
		}
	}

	return virq;
}

/**
 * irq_domain_reset_irq_data - Clear hwirq, chip and chip_data in @irq_data
 * @irq_data:	The pointer to irq_data
 */
void irq_domain_reset_irq_data(struct irq_data *irq_data)
{
	irq_data->hwirq = 0;
	irq_data->chip = &no_irq_chip;
	irq_data->chip_data = NULL;
}
EXPORT_SYMBOL_GPL(irq_domain_reset_irq_data);

#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY
/**
 * irq_domain_create_hierarchy - Add a irqdomain into the hierarchy
 * @parent:	Parent irq domain to associate with the new domain
 * @flags:	Irq domain flags associated to the domain
 * @size:	Size of the domain. See below
 * @fwnode:	Optional fwnode of the interrupt controller
 * @ops:	Pointer to the interrupt domain callbacks
 * @host_data:	Controller private data pointer
 *
 * If @size is 0 a tree domain is created, otherwise a linear domain.
 *
 * If successful the parent is associated to the new domain and the
 * domain flags are set.
 * Returns pointer to IRQ domain, or NULL on failure.
 */
struct irq_domain *irq_domain_create_hierarchy(struct irq_domain *parent,
					    unsigned int flags,
					    unsigned int size,
					    struct fwnode_handle *fwnode,
					    const struct irq_domain_ops *ops,
					    void *host_data)
{
	struct irq_domain *domain;

	if (size)
		domain = irq_domain_create_linear(fwnode, size, ops, host_data);
	else
		domain = irq_domain_create_tree(fwnode, ops, host_data);
	if (domain) {
		domain->parent = parent;
		domain->flags |= flags;
	}

	return domain;
}
EXPORT_SYMBOL_GPL(irq_domain_create_hierarchy);

/*
 * IAMROOT, 2022.10.15:
 * - @domain에 virq의 hwirq를 key로 irq_data를 mapping한다.
 */
static void irq_domain_insert_irq(int virq)
{
	struct irq_data *data;

	for (data = irq_get_irq_data(virq); data; data = data->parent_data) {
		struct irq_domain *domain = data->domain;

		domain->mapcount++;
		irq_domain_set_mapping(domain, data->hwirq, data);

		/* If not already assigned, give the domain the chip's name */
		if (!domain->name && data->chip)
			domain->name = data->chip->name;
	}

/*
 * IAMROOT, 2022.10.15:
 * - 자료구조가 완성됬고, 이제 사용자 요청만을 기다리면 되는 상태.
 */
	irq_clear_status_flags(virq, IRQ_NOREQUEST);
}

static void irq_domain_remove_irq(int virq)
{
	struct irq_data *data;

	irq_set_status_flags(virq, IRQ_NOREQUEST);
	irq_set_chip_and_handler(virq, NULL, NULL);
	synchronize_irq(virq);
	smp_mb();

	for (data = irq_get_irq_data(virq); data; data = data->parent_data) {
		struct irq_domain *domain = data->domain;
		irq_hw_number_t hwirq = data->hwirq;

		domain->mapcount--;
		irq_domain_clear_mapping(domain, hwirq);
	}
}

/*
 * IAMROOT, 2022.10.15:
 * @domain parent domain
 *
 * - @child의 parent irq_data를 생성한다.
 *   child의 irq, common을 동일한 값을 가져간다.
 */
static struct irq_data *irq_domain_insert_irq_data(struct irq_domain *domain,
						   struct irq_data *child)
{
	struct irq_data *irq_data;

	irq_data = kzalloc_node(sizeof(*irq_data), GFP_KERNEL,
				irq_data_get_node(child));
	if (irq_data) {
		child->parent_data = irq_data;
		irq_data->irq = child->irq;
		irq_data->common = child->common;
		irq_data->domain = domain;
	}

	return irq_data;
}

/*
 * IAMROOT, 2022.10.15:
 * - irq_data 부터 모든 parent를 free한다.
 */
static void __irq_domain_free_hierarchy(struct irq_data *irq_data)
{
	struct irq_data *tmp;

	while (irq_data) {
		tmp = irq_data;
		irq_data = irq_data->parent_data;
		kfree(tmp);
	}
}

static void irq_domain_free_irq_data(unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *irq_data, *tmp;
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_data = irq_get_irq_data(virq + i);
		tmp = irq_data->parent_data;
		irq_data->parent_data = NULL;
		irq_data->domain = NULL;

		__irq_domain_free_hierarchy(tmp);
	}
}

/**
 * irq_domain_disconnect_hierarchy - Mark the first unused level of a hierarchy
 * @domain:	IRQ domain from which the hierarchy is to be disconnected
 * @virq:	IRQ number where the hierarchy is to be trimmed
 *
 * Marks the @virq level belonging to @domain as disconnected.
 * Returns -EINVAL if @virq doesn't have a valid irq_data pointing
 * to @domain.
 *
 * Its only use is to be able to trim levels of hierarchy that do not
 * have any real meaning for this interrupt, and that the driver marks
 * as such from its .alloc() callback.
 */
/*
 * IAMROOT, 2022.10.15:
 * - 일부 hwirq가 cascade처리를 안하는 경우가 있다. 이 경우 empty function등을
 *   해서 fake 계층 처리를 해도 되지만, 이렇게 복잡하게 안하고 ENOTCONN을
 *   사용해 계층을 축소 시키는 방법이 추가 됬다.
 */
int irq_domain_disconnect_hierarchy(struct irq_domain *domain,
				    unsigned int virq)
{
	struct irq_data *irqd;

	irqd = irq_domain_get_irq_data(domain, virq);
	if (!irqd)
		return -EINVAL;

	irqd->chip = ERR_PTR(-ENOTCONN);
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_disconnect_hierarchy);

/*
 * IAMROOT, 2022.10.15:
 * - ENOTCONN된 parent irq_data을 찾아서 trim(free) 시킨다.
 */
static int irq_domain_trim_hierarchy(unsigned int virq)
{
	struct irq_data *tail, *irqd, *irq_data;

	irq_data = irq_get_irq_data(virq);
	tail = NULL;

	/* The first entry must have a valid irqchip */
	if (!irq_data->chip || IS_ERR(irq_data->chip))
		return -EINVAL;

	/*
	 * Validate that the irq_data chain is sane in the presence of
	 * a hierarchy trimming marker.
	 */
	for (irqd = irq_data->parent_data; irqd; irq_data = irqd, irqd = irqd->parent_data) {
		/* Can't have a valid irqchip after a trim marker */
		if (irqd->chip && tail)
			return -EINVAL;

		/* Can't have an empty irqchip before a trim marker */
		if (!irqd->chip && !tail)
			return -EINVAL;

		if (IS_ERR(irqd->chip)) {
			/* Only -ENOTCONN is a valid trim marker */
			if (PTR_ERR(irqd->chip) != -ENOTCONN)
				return -EINVAL;

/*
 * IAMROOT, 2022.10.15:
 * - irqd->chip ENOTCONN가 있는 마지막 irq_data를 찾는다.
 */
			tail = irq_data;
		}
	}

	/* No trim marker, nothing to do */
	if (!tail)
		return 0;

/*
 * IAMROOT, 2022.10.15:
 * - ENOTCONN가 된 irqd에서 trim을 시키고 trim된 irqd의 parent들을 전부 free.
 */
	pr_info("IRQ%d: trimming hierarchy from %s\n",
		virq, tail->parent_data->domain->name);

	/* Sever the inner part of the hierarchy...  */
	irqd = tail;
	tail = tail->parent_data;
	irqd->parent_data = NULL;

	__irq_domain_free_hierarchy(tail);

	return 0;
}

/*
 * IAMROOT, 2022.10.15:
 * - @virq부터 nr_irqs수의 irq data에 @domain을 등록한다. @domain이 계층 구조인경우
 *   타고 올라가면서 irq_data를 할당해준다.(부분적 설정. 일단 할당만 하는 개념)
 */
static int irq_domain_alloc_irq_data(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *irq_data;
	struct irq_domain *parent;
	int i;

/*
 * IAMROOT, 2022.10.15:
 * - 첫번째 desc에는 irq_data가 이미 embed 되있다. 도메인이 계층구조인 경우엔
 *   irq_data도 동일하게 parent 구조로 등록시켜준다.
 */
	/* The outermost irq_data is embedded in struct irq_desc */
	for (i = 0; i < nr_irqs; i++) {
		irq_data = irq_get_irq_data(virq + i);
		irq_data->domain = domain;

/*
 * IAMROOT, 2022.10.15:
 * - 계층구조라면 위로 올라가면서 등록시켜준다.
 *   irq_data는 domain, common, irq만을 일당 할당해준다.
 */
		for (parent = domain->parent; parent; parent = parent->parent) {
			irq_data = irq_domain_insert_irq_data(parent, irq_data);
			if (!irq_data) {
				irq_domain_free_irq_data(virq, i + 1);
				return -ENOMEM;
			}
		}
	}

	return 0;
}

/**
 * irq_domain_get_irq_data - Get irq_data associated with @virq and @domain
 * @domain:	domain to match
 * @virq:	IRQ number to get irq_data
 */

/*
 * IAMROOT, 2022.10.01:
 * - child부터 시작해서 parent에 거슬러 올라가며 domain이 같은 irq_data를
 *   return 한다.
 */
struct irq_data *irq_domain_get_irq_data(struct irq_domain *domain,
					 unsigned int virq)
{
	struct irq_data *irq_data;

	for (irq_data = irq_get_irq_data(virq); irq_data;
	     irq_data = irq_data->parent_data)
		if (irq_data->domain == domain)
			return irq_data;

	return NULL;
}
EXPORT_SYMBOL_GPL(irq_domain_get_irq_data);

/**
 * irq_domain_set_hwirq_and_chip - Set hwirq and irqchip of @virq at @domain
 * @domain:	Interrupt domain to match
 * @virq:	IRQ number
 * @hwirq:	The hwirq number
 * @chip:	The associated interrupt chip
 * @chip_data:	The associated chip data
 */
/*
 * IAMROOT, 2022.10.01:
 * - virq를 가진 irq_data 계층에서 @domain과 일치하는 irq_data를 가져오고,
 *   해당 irq_data에 @hwirq, @chip, @chip_data를 넣는다.
 */
int irq_domain_set_hwirq_and_chip(struct irq_domain *domain, unsigned int virq,
				  irq_hw_number_t hwirq, struct irq_chip *chip,
				  void *chip_data)
{
	struct irq_data *irq_data = irq_domain_get_irq_data(domain, virq);

	if (!irq_data)
		return -ENOENT;

	irq_data->hwirq = hwirq;
	irq_data->chip = chip ? chip : &no_irq_chip;
	irq_data->chip_data = chip_data;

	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_set_hwirq_and_chip);

/**
 * irq_domain_set_info - Set the complete data for a @virq in @domain
 * @domain:		Interrupt domain to match
 * @virq:		IRQ number
 * @hwirq:		The hardware interrupt number
 * @chip:		The associated interrupt chip
 * @chip_data:		The associated interrupt chip data
 * @handler:		The interrupt flow handler
 * @handler_data:	The interrupt flow handler data
 * @handler_name:	The interrupt handler name
 */
/*
 * IAMROOT, 2022.10.01:
 * - @virq에 대한 @domain을 찾아서 irq_data에 @chip, @chip_datg,
 *   @hwirq등을 등록하고 @handler, @handler_data등을 설정하고 @virq를
 *   activate 및 startup을 한다.
 */
void irq_domain_set_info(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq, struct irq_chip *chip,
			 void *chip_data, irq_flow_handler_t handler,
			 void *handler_data, const char *handler_name)
{
	irq_domain_set_hwirq_and_chip(domain, virq, hwirq, chip, chip_data);
	__irq_set_handler(virq, handler, 0, handler_name);
	irq_set_handler_data(virq, handler_data);
}
EXPORT_SYMBOL(irq_domain_set_info);

/**
 * irq_domain_free_irqs_common - Clear irq_data and free the parent
 * @domain:	Interrupt domain to match
 * @virq:	IRQ number to start with
 * @nr_irqs:	The number of irqs to free
 */
void irq_domain_free_irqs_common(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs)
{
	struct irq_data *irq_data;
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_data = irq_domain_get_irq_data(domain, virq + i);
		if (irq_data)
			irq_domain_reset_irq_data(irq_data);
	}
	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
}
EXPORT_SYMBOL_GPL(irq_domain_free_irqs_common);

/**
 * irq_domain_free_irqs_top - Clear handler and handler data, clear irqdata and free parent
 * @domain:	Interrupt domain to match
 * @virq:	IRQ number to start with
 * @nr_irqs:	The number of irqs to free
 */
void irq_domain_free_irqs_top(struct irq_domain *domain, unsigned int virq,
			      unsigned int nr_irqs)
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_set_handler_data(virq + i, NULL);
		irq_set_handler(virq + i, NULL);
	}
	irq_domain_free_irqs_common(domain, virq, nr_irqs);
}

static void irq_domain_free_irqs_hierarchy(struct irq_domain *domain,
					   unsigned int irq_base,
					   unsigned int nr_irqs)
{
	unsigned int i;

	if (!domain->ops->free)
		return;

	for (i = 0; i < nr_irqs; i++) {
		if (irq_domain_get_irq_data(domain, irq_base + i))
			domain->ops->free(domain, irq_base + i, 1);
	}
}

/*
 * IAMROOT, 2022.10.15:
 * - @domain의 alloc ops를 호출해서 mapping한다.
 * - ex)
 *   gic_irq_domain_alloc. arg는 fwspec.
 */
int irq_domain_alloc_irqs_hierarchy(struct irq_domain *domain,
				    unsigned int irq_base,
				    unsigned int nr_irqs, void *arg)
{
	if (!domain->ops->alloc) {
		pr_debug("domain->ops->alloc() is NULL\n");
		return -ENOSYS;
	}

	return domain->ops->alloc(domain, irq_base, nr_irqs, arg);
}

/**
 * __irq_domain_alloc_irqs - Allocate IRQs from domain
 * @domain:	domain to allocate from
 * @irq_base:	allocate specified IRQ number if irq_base >= 0
 * @nr_irqs:	number of IRQs to allocate
 * @node:	NUMA node id for memory allocation
 * @arg:	domain specific argument
 * @realloc:	IRQ descriptors have already been allocated if true
 * @affinity:	Optional irq affinity mask for multiqueue devices
 *
 * Allocate IRQ numbers and initialized all data structures to support
 * hierarchy IRQ domains.
 * Parameter @realloc is mainly to support legacy IRQs.
 * Returns error code or allocated IRQ number
 *
 * The whole process to setup an IRQ has been split into two steps.
 * The first step, __irq_domain_alloc_irqs(), is to allocate IRQ
 * descriptor and required hardware resources. The second step,
 * irq_domain_activate_irq(), is to program the hardware with preallocated
 * resources. In this way, it's easier to rollback when failing to
 * allocate resources.
 */
/*
 * IAMROOT, 2022.10.15:
 * - papago
 *   계층 구조 IRQ 도메인을 지원하기 위해 IRQ 번호를 할당하고 모든 데이터 구조를
 *   초기화합니다.
 *   @realloc 매개변수는 주로 레거시 IRQ를 지원하기 위한 것입니다.
 *   오류 코드 또는 할당된 IRQ 번호를 반환합니다. IRQ를 설정하는 전체 프로세스는
 *   두 단계로 분할되었습니다.
 *   첫 번째 단계인 __irq_domain_alloc_irqs()는 IRQ 설명자와 필요한 하드웨어
 *   리소스를 할당하는 것입니다. 두 번째 단계인 irq_domain_activate_irq()는 미리
 *   할당된 리소스로 하드웨어를 프로그래밍하는 것입니다. 이런 식으로 리소스 할당에
 *   실패했을 때 롤백하기가 더 쉽습니다.
 *
 * - irq_desc 할당(irq_desc radix에 virq로 insert)(realloc == false인경우)
 *   -> irq data를 domain 에 맞게 할당
 *   -> domain alloc ops로 virq mapping
 *   -> ENOTCONN된 parent irq_data trim
 *   -> domain에 irq_data추가(domain revmap에 hwirq를 key로 irq_data mapping.)
 */
int __irq_domain_alloc_irqs(struct irq_domain *domain, int irq_base,
			    unsigned int nr_irqs, int node, void *arg,
			    bool realloc, const struct irq_affinity_desc *affinity)
{
	int i, ret, virq;

	if (domain == NULL) {
		domain = irq_default_domain;
		if (WARN(!domain, "domain is NULL; cannot allocate IRQ\n"))
			return -EINVAL;
	}

/*
 * IAMROOT, 2022.10.15:
 * - realloc == true 
 *   @irq_base가 이미 할당되 있다고 생각하고 있는경우. irq_desc가 생성되있기
 *   때문에 desc 자체는 생성할 필요가없다.
 * - realloc == false, irq_base >= 0
 *   irq_base에 해당하는 irq desc가 없다고 생각해서, irq_base에 해당하는 desc도
 *   만들어야 된다.
 * - irq_base < 0
 *   0번부터 @nr_irqs개수만큼 virq를 만들어서 desc부터 만들어야된다.
 */
	if (realloc && irq_base >= 0) {
		virq = irq_base;
	} else {

/*
 * IAMROOT, 2022.10.15:
 * - irq desc를 만든다.
 */
		virq = irq_domain_alloc_descs(irq_base, nr_irqs, 0, node,
					      affinity);
		if (virq < 0) {
			pr_debug("cannot allocate IRQ(base %d, count %d)\n",
				 irq_base, nr_irqs);
			return virq;
		}
	}

/*
 * IAMROOT, 2022.10.15:
 * - 위에서 만들어진 or 이미 존재하던 irq desc의 irq_data들을 domain 계층 구조에 맞게
 *   할당한다.
 */
	if (irq_domain_alloc_irq_data(domain, virq, nr_irqs)) {
		pr_debug("cannot allocate memory for IRQ%d\n", virq);
		ret = -ENOMEM;
		goto out_free_desc;
	}

	mutex_lock(&irq_domain_mutex);

/*
 * IAMROOT, 2022.10.15:
 * - domain alloc ops로 domain에 mapping한다. (hwirq가 mapping된다.)
 */
	ret = irq_domain_alloc_irqs_hierarchy(domain, virq, nr_irqs, arg);
	if (ret < 0) {
		mutex_unlock(&irq_domain_mutex);
		goto out_free_irq_data;
	}

/*
 * IAMROOT, 2022.10.15:
 * - ENOTCONN된 parent irq_data가 있는지 한번 검색하고, 있으면 trim
 */
	for (i = 0; i < nr_irqs; i++) {
		ret = irq_domain_trim_hierarchy(virq + i);
		if (ret) {
			mutex_unlock(&irq_domain_mutex);
			goto out_free_irq_data;
		}
	}
	
/*
 * IAMROOT, 2022.10.15:
 * - domain에 virq를 mapping한다.
 */
	for (i = 0; i < nr_irqs; i++)
		irq_domain_insert_irq(virq + i);
	mutex_unlock(&irq_domain_mutex);

	return virq;

out_free_irq_data:
	irq_domain_free_irq_data(virq, nr_irqs);
out_free_desc:
	irq_free_descs(virq, nr_irqs);
	return ret;
}

/* The irq_data was moved, fix the revmap to refer to the new location */
static void irq_domain_fix_revmap(struct irq_data *d)
{
	void __rcu **slot;

	if (irq_domain_is_nomap(d->domain))
		return;

	/* Fix up the revmap. */
	mutex_lock(&d->domain->revmap_mutex);
	if (d->hwirq < d->domain->revmap_size) {
		/* Not using radix tree */
		rcu_assign_pointer(d->domain->revmap[d->hwirq], d);
	} else {
		slot = radix_tree_lookup_slot(&d->domain->revmap_tree, d->hwirq);
		if (slot)
			radix_tree_replace_slot(&d->domain->revmap_tree, slot, d);
	}
	mutex_unlock(&d->domain->revmap_mutex);
}

/**
 * irq_domain_push_irq() - Push a domain in to the top of a hierarchy.
 * @domain:	Domain to push.
 * @virq:	Irq to push the domain in to.
 * @arg:	Passed to the irq_domain_ops alloc() function.
 *
 * For an already existing irqdomain hierarchy, as might be obtained
 * via a call to pci_enable_msix(), add an additional domain to the
 * head of the processing chain.  Must be called before request_irq()
 * has been called.
 */
int irq_domain_push_irq(struct irq_domain *domain, int virq, void *arg)
{
	struct irq_data *child_irq_data;
	struct irq_data *root_irq_data = irq_get_irq_data(virq);
	struct irq_desc *desc;
	int rv = 0;

	/*
	 * Check that no action has been set, which indicates the virq
	 * is in a state where this function doesn't have to deal with
	 * races between interrupt handling and maintaining the
	 * hierarchy.  This will catch gross misuse.  Attempting to
	 * make the check race free would require holding locks across
	 * calls to struct irq_domain_ops->alloc(), which could lead
	 * to deadlock, so we just do a simple check before starting.
	 */
	desc = irq_to_desc(virq);
	if (!desc)
		return -EINVAL;
	if (WARN_ON(desc->action))
		return -EBUSY;

	if (domain == NULL)
		return -EINVAL;

	if (WARN_ON(!irq_domain_is_hierarchy(domain)))
		return -EINVAL;

	if (!root_irq_data)
		return -EINVAL;

	if (domain->parent != root_irq_data->domain)
		return -EINVAL;

	child_irq_data = kzalloc_node(sizeof(*child_irq_data), GFP_KERNEL,
				      irq_data_get_node(root_irq_data));
	if (!child_irq_data)
		return -ENOMEM;

	mutex_lock(&irq_domain_mutex);

	/* Copy the original irq_data. */
	*child_irq_data = *root_irq_data;

	/*
	 * Overwrite the root_irq_data, which is embedded in struct
	 * irq_desc, with values for this domain.
	 */
	root_irq_data->parent_data = child_irq_data;
	root_irq_data->domain = domain;
	root_irq_data->mask = 0;
	root_irq_data->hwirq = 0;
	root_irq_data->chip = NULL;
	root_irq_data->chip_data = NULL;

	/* May (probably does) set hwirq, chip, etc. */
	rv = irq_domain_alloc_irqs_hierarchy(domain, virq, 1, arg);
	if (rv) {
		/* Restore the original irq_data. */
		*root_irq_data = *child_irq_data;
		kfree(child_irq_data);
		goto error;
	}

	irq_domain_fix_revmap(child_irq_data);
	irq_domain_set_mapping(domain, root_irq_data->hwirq, root_irq_data);

error:
	mutex_unlock(&irq_domain_mutex);

	return rv;
}
EXPORT_SYMBOL_GPL(irq_domain_push_irq);

/**
 * irq_domain_pop_irq() - Remove a domain from the top of a hierarchy.
 * @domain:	Domain to remove.
 * @virq:	Irq to remove the domain from.
 *
 * Undo the effects of a call to irq_domain_push_irq().  Must be
 * called either before request_irq() or after free_irq().
 */
int irq_domain_pop_irq(struct irq_domain *domain, int virq)
{
	struct irq_data *root_irq_data = irq_get_irq_data(virq);
	struct irq_data *child_irq_data;
	struct irq_data *tmp_irq_data;
	struct irq_desc *desc;

	/*
	 * Check that no action is set, which indicates the virq is in
	 * a state where this function doesn't have to deal with races
	 * between interrupt handling and maintaining the hierarchy.
	 * This will catch gross misuse.  Attempting to make the check
	 * race free would require holding locks across calls to
	 * struct irq_domain_ops->free(), which could lead to
	 * deadlock, so we just do a simple check before starting.
	 */
	desc = irq_to_desc(virq);
	if (!desc)
		return -EINVAL;
	if (WARN_ON(desc->action))
		return -EBUSY;

	if (domain == NULL)
		return -EINVAL;

	if (!root_irq_data)
		return -EINVAL;

	tmp_irq_data = irq_domain_get_irq_data(domain, virq);

	/* We can only "pop" if this domain is at the top of the list */
	if (WARN_ON(root_irq_data != tmp_irq_data))
		return -EINVAL;

	if (WARN_ON(root_irq_data->domain != domain))
		return -EINVAL;

	child_irq_data = root_irq_data->parent_data;
	if (WARN_ON(!child_irq_data))
		return -EINVAL;

	mutex_lock(&irq_domain_mutex);

	root_irq_data->parent_data = NULL;

	irq_domain_clear_mapping(domain, root_irq_data->hwirq);
	irq_domain_free_irqs_hierarchy(domain, virq, 1);

	/* Restore the original irq_data. */
	*root_irq_data = *child_irq_data;

	irq_domain_fix_revmap(root_irq_data);

	mutex_unlock(&irq_domain_mutex);

	kfree(child_irq_data);

	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_pop_irq);

/**
 * irq_domain_free_irqs - Free IRQ number and associated data structures
 * @virq:	base IRQ number
 * @nr_irqs:	number of IRQs to free
 */
void irq_domain_free_irqs(unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *data = irq_get_irq_data(virq);
	int i;

	if (WARN(!data || !data->domain || !data->domain->ops->free,
		 "NULL pointer, cannot free irq\n"))
		return;

	mutex_lock(&irq_domain_mutex);
	for (i = 0; i < nr_irqs; i++)
		irq_domain_remove_irq(virq + i);
	irq_domain_free_irqs_hierarchy(data->domain, virq, nr_irqs);
	mutex_unlock(&irq_domain_mutex);

	irq_domain_free_irq_data(virq, nr_irqs);
	irq_free_descs(virq, nr_irqs);
}

/**
 * irq_domain_alloc_irqs_parent - Allocate interrupts from parent domain
 * @domain:	Domain below which interrupts must be allocated
 * @irq_base:	Base IRQ number
 * @nr_irqs:	Number of IRQs to allocate
 * @arg:	Allocation data (arch/domain specific)
 */
int irq_domain_alloc_irqs_parent(struct irq_domain *domain,
				 unsigned int irq_base, unsigned int nr_irqs,
				 void *arg)
{
	if (!domain->parent)
		return -ENOSYS;

	return irq_domain_alloc_irqs_hierarchy(domain->parent, irq_base,
					       nr_irqs, arg);
}
EXPORT_SYMBOL_GPL(irq_domain_alloc_irqs_parent);

/**
 * irq_domain_free_irqs_parent - Free interrupts from parent domain
 * @domain:	Domain below which interrupts must be freed
 * @irq_base:	Base IRQ number
 * @nr_irqs:	Number of IRQs to free
 */
void irq_domain_free_irqs_parent(struct irq_domain *domain,
				 unsigned int irq_base, unsigned int nr_irqs)
{
	if (!domain->parent)
		return;

	irq_domain_free_irqs_hierarchy(domain->parent, irq_base, nr_irqs);
}
EXPORT_SYMBOL_GPL(irq_domain_free_irqs_parent);

static void __irq_domain_deactivate_irq(struct irq_data *irq_data)
{
	if (irq_data && irq_data->domain) {
		struct irq_domain *domain = irq_data->domain;

		if (domain->ops->deactivate)
			domain->ops->deactivate(domain, irq_data);
		if (irq_data->parent_data)
			__irq_domain_deactivate_irq(irq_data->parent_data);
	}
}

/*
 * IAMROOT, 2022.10.01:
 * - parent부터 activate. gic v3 its같은경우엔 actiate ops가 존재한다.
 */
static int __irq_domain_activate_irq(struct irq_data *irqd, bool reserve)
{
	int ret = 0;

	if (irqd && irqd->domain) {
		struct irq_domain *domain = irqd->domain;

/*
 * IAMROOT, 2022.10.01:
 * - parent 재귀.
 */
		if (irqd->parent_data)
			ret = __irq_domain_activate_irq(irqd->parent_data,
							reserve);
/*
 * IAMROOT, 2022.10.01:
 * - parent에서 성공했으면 수행한다.
 * - activate가 있으면 activate.
 *   ops ex) its_domain_ops
 */
		if (!ret && domain->ops->activate) {
			ret = domain->ops->activate(domain, irqd, reserve);
			/* Rollback in case of error */
/*
 * IAMROOT, 2022.10.01:
 * - 하나라도 실패하면 다시 타고올라가면서 deactivate.
 */
			if (ret && irqd->parent_data)
				__irq_domain_deactivate_irq(irqd->parent_data);
		}
	}
	return ret;
}

/**
 * irq_domain_activate_irq - Call domain_ops->activate recursively to activate
 *			     interrupt
 * @irq_data:	Outermost irq_data associated with interrupt
 * @reserve:	If set only reserve an interrupt vector instead of assigning one
 *
 * This is the second step to call domain_ops->activate to program interrupt
 * controllers, so the interrupt could actually get delivered.
 */
/*
 * IAMROOT, 2022.10.01:
 * - activate 안되있면 activate 수행. actiavte는 gic v3 its.
 *   activate가 정상적으로 수행되면 irq_data에 activate bit를 set한다.
 */
int irq_domain_activate_irq(struct irq_data *irq_data, bool reserve)
{
	int ret = 0;

	if (!irqd_is_activated(irq_data))
		ret = __irq_domain_activate_irq(irq_data, reserve);
	if (!ret)
		irqd_set_activated(irq_data);
	return ret;
}

/**
 * irq_domain_deactivate_irq - Call domain_ops->deactivate recursively to
 *			       deactivate interrupt
 * @irq_data: outermost irq_data associated with interrupt
 *
 * It calls domain_ops->deactivate to program interrupt controllers to disable
 * interrupt delivery.
 */
void irq_domain_deactivate_irq(struct irq_data *irq_data)
{
	if (irqd_is_activated(irq_data)) {
		__irq_domain_deactivate_irq(irq_data);
		irqd_clr_activated(irq_data);
	}
}

/*
 * IAMROOT, 2022.10.01:
 * - 계층구조로 만들 필요가 있는지를 표시
 * - ex)
 *    hw1 --> hw2
 *        \-> hw3
 */
static void irq_domain_check_hierarchy(struct irq_domain *domain)
{
	/* Hierarchy irq_domains must implement callback alloc() */
	if (domain->ops->alloc)
		domain->flags |= IRQ_DOMAIN_FLAG_HIERARCHY;
}

/**
 * irq_domain_hierarchical_is_msi_remap - Check if the domain or any
 * parent has MSI remapping support
 * @domain: domain pointer
 */
bool irq_domain_hierarchical_is_msi_remap(struct irq_domain *domain)
{
	for (; domain; domain = domain->parent) {
		if (irq_domain_is_msi_remap(domain))
			return true;
	}
	return false;
}
#else	/* CONFIG_IRQ_DOMAIN_HIERARCHY */
/**
 * irq_domain_get_irq_data - Get irq_data associated with @virq and @domain
 * @domain:	domain to match
 * @virq:	IRQ number to get irq_data
 */
struct irq_data *irq_domain_get_irq_data(struct irq_domain *domain,
					 unsigned int virq)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);

	return (irq_data && irq_data->domain == domain) ? irq_data : NULL;
}
EXPORT_SYMBOL_GPL(irq_domain_get_irq_data);

/**
 * irq_domain_set_info - Set the complete data for a @virq in @domain
 * @domain:		Interrupt domain to match
 * @virq:		IRQ number
 * @hwirq:		The hardware interrupt number
 * @chip:		The associated interrupt chip
 * @chip_data:		The associated interrupt chip data
 * @handler:		The interrupt flow handler
 * @handler_data:	The interrupt flow handler data
 * @handler_name:	The interrupt handler name
 */
void irq_domain_set_info(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq, struct irq_chip *chip,
			 void *chip_data, irq_flow_handler_t handler,
			 void *handler_data, const char *handler_name)
{
	irq_set_chip_and_handler_name(virq, chip, handler, handler_name);
	irq_set_chip_data(virq, chip_data);
	irq_set_handler_data(virq, handler_data);
}

static void irq_domain_check_hierarchy(struct irq_domain *domain)
{
}
#endif	/* CONFIG_IRQ_DOMAIN_HIERARCHY */

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
static struct dentry *domain_dir;

static void
irq_domain_debug_show_one(struct seq_file *m, struct irq_domain *d, int ind)
{
	seq_printf(m, "%*sname:   %s\n", ind, "", d->name);
	seq_printf(m, "%*ssize:   %u\n", ind + 1, "", d->revmap_size);
	seq_printf(m, "%*smapped: %u\n", ind + 1, "", d->mapcount);
	seq_printf(m, "%*sflags:  0x%08x\n", ind +1 , "", d->flags);
	if (d->ops && d->ops->debug_show)
		d->ops->debug_show(m, d, NULL, ind + 1);
#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY
	if (!d->parent)
		return;
	seq_printf(m, "%*sparent: %s\n", ind + 1, "", d->parent->name);
	irq_domain_debug_show_one(m, d->parent, ind + 4);
#endif
}

static int irq_domain_debug_show(struct seq_file *m, void *p)
{
	struct irq_domain *d = m->private;

	/* Default domain? Might be NULL */
	if (!d) {
		if (!irq_default_domain)
			return 0;
		d = irq_default_domain;
	}
	irq_domain_debug_show_one(m, d, 0);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(irq_domain_debug);

static void debugfs_add_domain_dir(struct irq_domain *d)
{
	if (!d->name || !domain_dir)
		return;
	debugfs_create_file(d->name, 0444, domain_dir, d,
			    &irq_domain_debug_fops);
}

static void debugfs_remove_domain_dir(struct irq_domain *d)
{
	debugfs_remove(debugfs_lookup(d->name, domain_dir));
}

void __init irq_domain_debugfs_init(struct dentry *root)
{
	struct irq_domain *d;

	domain_dir = debugfs_create_dir("domains", root);

	debugfs_create_file("default", 0444, domain_dir, NULL,
			    &irq_domain_debug_fops);
	mutex_lock(&irq_domain_mutex);
	list_for_each_entry(d, &irq_domain_list, link)
		debugfs_add_domain_dir(d);
	mutex_unlock(&irq_domain_mutex);
}
#endif
