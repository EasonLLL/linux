/*
 * Author: wowo<wowo@wowotech.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/serial_core.h>
#include <linux/console.h>
#include <linux/tty_flip.h>
#include <asm/io.h>

#define UART5_BASE		(0xE012a000)

#define UART_CTL		(0x0)
#define UART_RXDAT		(0x4)
#define	UART_TXDAT		(0x8)
#define	UART_STAT		(0xc)

#define UART_CTL_TXIE		(0x1 << 19)	/* UART TX IRQ Enable. */
#define UART_CTL_RXIE		(0x1 << 18)	/* UART RX IRQ Enable. */
#define UART_CTL_EN		(0x1 << 15)	/* UART Enable. */

#define UART_STAT_UTBB		(0x1 << 17)	/* UART TX busy bit */
#define UART_STAT_TFES		(0x1 << 10)	/* TX FIFO empty */
#define UART_STAT_RFFS		(0x1 << 9)	/* RX FIFO full */
#define UART_STAT_TFFU		(0x1 << 6)	/* TX FIFO full */
#define UART_STAT_RFEM		(0x1 << 5)	/* RX FIFO Empty */
#define UART_STAT_TIP		(0x1 << 1)	/* TX IRQ Pending */
#define UART_STAT_RIP		(0x1 << 0)	/* RX IRQ Pending */

/*============================================================================
 *			old earlycon, will be removed later
 *==========================================================================*/

/* TODO */
static void __iomem		*uart5_base;

/* TODO, FIXME!!! */
static void owl_serial_putc(struct uart_port *port, int ch)
{
	/* wait for TX FIFO untill it is not full */
	while (readl(uart5_base + UART_STAT) & UART_STAT_TFFU)
		;

	writel(ch, uart5_base + UART_TXDAT);
}

static void earlycon_owl_write(struct console *con, const char *s, unsigned n)
{
	/* TODO */
	uart_console_write(NULL, s, n, owl_serial_putc);
}

int __init earlycon_owl_setup(struct earlycon_device *device, const char *opt)
{
	/* TODO */
	uart5_base = early_ioremap(UART5_BASE, 4);

	device->con->write = earlycon_owl_write;
	return 0;
}
EARLYCON_DECLARE(owl_serial, earlycon_owl_setup);


/*============================================================================
 *				new serial driver
 *==========================================================================*/
#define OWL_SERIAL_MAXIMUM		6

struct owl_uart_port {
	struct uart_port		port;

	/* others, TODO */
};
#define PORT_TO_OUP(port)		container_of(port, struct owl_uart_port, port)

/*============================================================================
 *				UART operations
 *==========================================================================*/
#define __PORT_SET_BIT(port, reg, bit)			\
	writel(readl((port)->membase + (reg)) | (bit),	\
	       (port)->membase + (reg))

#define __PORT_CLEAR_BIT(port, reg, bit)		\
	writel(readl((port)->membase + (reg)) & ~(bit),	\
	       (port)->membase + (reg))

#define __PORT_TEST_BIT(port, reg, bit)			\
	(!!(readl((port)->membase + (reg)) & (bit)))

static void __write_tx_fifo(struct uart_port *port)
{
	struct circ_buf *xmit = &port->state->xmit;

	/* write TX FIFO while: TX FIFO not full and TX buffer no empty */
	while (1) {
		if (uart_circ_empty(xmit)) {
			/* no data sent, no IRQ need */
			__PORT_CLEAR_BIT(port, UART_CTL, UART_CTL_TXIE);
			break;
		} else {
			if (__PORT_TEST_BIT(port, UART_STAT, UART_STAT_TFFU)) {
				/* TX FIFO full, enable IRQ for next transmit */
				__PORT_SET_BIT(port, UART_CTL, UART_CTL_TXIE);
				break;
			} else {
				writel(xmit->buf[xmit->tail],
				       port->membase + UART_TXDAT);
				xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
				port->icount.tx++;
			}
		}
	}
}

static irqreturn_t owl_serial_irq_handle(int irq, void *data)
{
	unsigned char ch;

	struct uart_port *port = data;

	dev_dbg(port->dev, "%s, %x\n", __func__,
		readl(port->membase + UART_STAT));

	if (__PORT_TEST_BIT(port, UART_STAT, UART_STAT_TIP))
		__write_tx_fifo(port);	/* try to transmit */

	if (__PORT_TEST_BIT(port, UART_STAT, UART_STAT_RIP)) {
		while (!__PORT_TEST_BIT(port, UART_STAT, UART_STAT_RFEM)) {
			ch = readb(port->membase + UART_RXDAT);

			/* flag, TODO */
			tty_insert_flip_char(&port->state->port, ch, TTY_NORMAL);
		}
		tty_flip_buffer_push(&port->state->port);
	}

	/* clear TX/RX pending bit */
	__PORT_SET_BIT(port, UART_STAT, (UART_STAT_TIP | UART_STAT_RIP));

	return IRQ_HANDLED;
}

static int owl_serial_startup(struct uart_port *port)
{
	int ret = 0;

	dev_dbg(port->dev, "%s\n", __func__);

	ret = devm_request_irq(port->dev, port->irq, owl_serial_irq_handle,
			       IRQF_TRIGGER_HIGH, "owl_serial", port);
	if (ret < 0) {
		dev_err(port->dev, "request irq(%d) failed(%d)\n",
			port->irq, ret);
		return ret;
	}

	/* RX irq enable */
	__PORT_SET_BIT(port, UART_CTL, UART_CTL_RXIE);

	/* enable serial port */
	__PORT_SET_BIT(port, UART_CTL, UART_CTL_EN);

	return ret;
}

static void owl_serial_shutdown(struct uart_port *port)
{
	dev_dbg(port->dev, "%s\n", __func__);

	/* disable serial port */
	__PORT_CLEAR_BIT(port, UART_CTL, UART_CTL_EN);

	/* RX irq disable */
	__PORT_CLEAR_BIT(port, UART_CTL, UART_CTL_RXIE);

	devm_free_irq(port->dev, port->irq, port);
}

static void owl_serial_start_tx(struct uart_port *port)
{
	dev_dbg(port->dev, "%s\n", __func__);

	/* try to transmit */
	__write_tx_fifo(port);
}

static void owl_serial_stop_tx(struct uart_port *port)
{
	dev_dbg(port->dev, "%s\n", __func__);

	/* TX irq disable */
	__PORT_CLEAR_BIT(port, UART_CTL, UART_CTL_TXIE);

	/* wait TX complete */
	while (!__PORT_TEST_BIT(port, UART_STAT, UART_STAT_TFES));

	/* clear pending bit */
	__PORT_SET_BIT(port, UART_STAT, UART_STAT_TIP);
}

static void owl_serial_stop_rx(struct uart_port *port)
{
	unsigned char ch;

	dev_dbg(port->dev, "%s\n", __func__);

	/* RX irq disable */
	__PORT_CLEAR_BIT(port, UART_CTL, UART_CTL_RXIE);

	/* reset RX FIFO */
	while (!__PORT_TEST_BIT(port, UART_STAT, UART_STAT_RFEM))
		ch = (unsigned char)readl(port->membase + UART_RXDAT);

	/* clear pending bit */
	__PORT_SET_BIT(port, UART_STAT, UART_STAT_RIP);
}

static unsigned int owl_serial_tx_empty(struct uart_port *port)
{
	dev_dbg(port->dev, "%s\n", __func__);

	if (__PORT_TEST_BIT(port, UART_STAT, UART_STAT_TFES))
		return TIOCSER_TEMT;
	else
		return 0;
}

static void owl_serial_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	dev_dbg(port->dev, "%s\n", __func__);
}

static void owl_serial_set_termios(struct uart_port *port, struct ktermios *new,
				   struct ktermios *old)
{
	dev_dbg(port->dev, "%s\n", __func__);
}

static struct uart_ops owl_uart_ops = {
	.startup = owl_serial_startup,
	.shutdown = owl_serial_shutdown,
	.start_tx = owl_serial_start_tx,
	.stop_tx = owl_serial_stop_tx,
	.stop_rx = owl_serial_stop_rx,
	.tx_empty = owl_serial_tx_empty,
	.set_mctrl = owl_serial_set_mctrl,
	.set_termios = owl_serial_set_termios,
 };

/*============================================================================
 *				uart driver
 *==========================================================================*/
static struct console owl_console;

static struct uart_driver owl_serial_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "owl_serial",
	.dev_name	= "ttyS",
	.cons		= &owl_console,
	.nr		= OWL_SERIAL_MAXIMUM,
};

/*============================================================================
 *				console driver
 *==========================================================================*/
static void owl_console_putchar(struct uart_port *port, int ch)
{
	/* wait for TX FIFO untill it is not full */
	while (__PORT_TEST_BIT(port, UART_STAT, UART_STAT_TFFU))
		;

	writel(ch, port->membase + UART_TXDAT);
}

static void owl_console_write(struct console *con, const char *s, unsigned n)
{
	bool tx_irq_enabled;

	struct uart_driver *driver = con->data;
	struct uart_port *port = driver->state[con->index].uart_port;

	/* save TX IRQ status */
	tx_irq_enabled = __PORT_TEST_BIT(port, UART_CTL, UART_CTL_TXIE);

	/* TX irq disable */
	__PORT_CLEAR_BIT(port, UART_CTL, UART_CTL_TXIE);

	uart_console_write(port, s, n, owl_console_putchar);

	/* wait until all content have been sent out */
	while (__PORT_TEST_BIT(port, UART_STAT, UART_STAT_UTBB))
		;

	/* restore TX IRQ status */
	if (tx_irq_enabled)
		__PORT_SET_BIT(port, UART_CTL, UART_CTL_TXIE);
}

static int __init owl_console_setup(struct console *con, char *options)
{
	return 0;
}

static struct console owl_console = {
	.name	= "ttyS",
	.write	= owl_console_write,
	.device	= uart_console_device,
	.setup	= owl_console_setup,
	.flags	= CON_PRINTBUFFER,
	.index	= 5,
	.data	= &owl_serial_driver,
};

/*============================================================================
 *				platform driver
 *==========================================================================*/
static const struct of_device_id owl_serial_of_match[] = {
	{
		.compatible = "actions,s900-serial",
	},
	{
	},
};
MODULE_DEVICE_TABLE(of, owl_serial_of_match);

static int owl_serial_probe(struct platform_device *pdev)
{
	int ret = 0;

	struct owl_uart_port *oup;
	struct uart_port *port;

	const struct of_device_id *match;

	struct resource *resource;

	dev_info(&pdev->dev, "%s\n", __func__);

	match = of_match_device(owl_serial_of_match, &pdev->dev);
	if (!match) {
		dev_err(&pdev->dev, "Error: No device match found\n");
		return -ENODEV;
	}

	oup = devm_kzalloc(&pdev->dev, sizeof(*oup), GFP_KERNEL);
	if (oup == NULL) {
		dev_err(&pdev->dev, "Failed to allocate memory for oup\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, oup);
	port = &oup->port;

	port->dev = &pdev->dev;
	port->type = PORT_OWL;
	port->ops = &owl_uart_ops;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource) {
		dev_err(&pdev->dev, "No IO memory resource\n");
		return -ENODEV;
	}
	port->mapbase = resource->start;

	port->membase = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(port->membase)) {
		dev_err(&pdev->dev, "Failed to map memory resource\n");
		return PTR_ERR(port->membase);
	}
	port->iotype = UPIO_MEM32;

	port->irq = platform_get_irq(pdev, 0);
	if (port->irq < 0) {
		dev_err(&pdev->dev, "Failed to get irq\n");
		return port->irq;
	}

	port->line = of_alias_get_id(pdev->dev.of_node, "serial");
	if (port->line < 0) {
		dev_err(&pdev->dev, "failed to get alias id, errno %d\n",
			port->line);
		return port->line;
	}

	/* others, TODO */

	ret = uart_add_one_port(&owl_serial_driver, port);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to add uart port, err %d\n", ret);
		return ret;
	}

	return ret;
}

static int owl_serial_remove(struct platform_device *pdev)
{
	struct owl_uart_port *oup = platform_get_drvdata(pdev);
	struct uart_port *port = &oup->port;

	dev_info(&pdev->dev, "%s\n", __func__);

	uart_remove_one_port(&owl_serial_driver, port);

	return 0;
}

static struct platform_driver owl_serial_platform_driver = {
	.probe		= owl_serial_probe,
	.remove		= owl_serial_remove,
	.driver		= {
		.name	= "owl_serial",
		.of_match_table = owl_serial_of_match,
	},
};

static int __init owl_serial_init(void)
{
	int ret;

	pr_info("%s\n", __func__);

	ret = uart_register_driver(&owl_serial_driver);
	if (ret < 0) {
		pr_err("register %s uart driver failed\n",
		       owl_serial_driver.driver_name);
		return ret;
	}

	ret = platform_driver_register(&owl_serial_platform_driver);
	if (ret < 0) {
		pr_err("owl_serial_platform_driver register failed, ret = %d\n", ret);
		uart_unregister_driver(&owl_serial_driver);

		return ret;
	}

	return 0;
}

static void __exit owl_serial_exit(void)
{
	pr_info("%s\n", __func__);

	platform_driver_unregister(&owl_serial_platform_driver);
	uart_unregister_driver(&owl_serial_driver);
}

module_init(owl_serial_init);
module_exit(owl_serial_exit);

MODULE_ALIAS("platform driver: owl_serial");
MODULE_DESCRIPTION("serial driver for S900 soc");
MODULE_AUTHOR("wowo<wowo@wowotech.net>");
MODULE_LICENSE("GPL v2");
