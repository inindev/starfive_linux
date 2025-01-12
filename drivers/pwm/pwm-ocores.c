// SPDX-License-Identifier: GPL-2.0
/*
 * OpenCores PWM Driver
 *
 * https://opencores.org/projects/ptc
 *
 * Copyright (C) 2018-2023 StarFive Technology Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/reset.h>
#include <linux/slab.h>

#define REG_OCPWM_CNTR(base)	((base))
#define REG_OCPWM_HRC(base)	((base) + 0x4)
#define REG_OCPWM_LRC(base)	((base) + 0x8)
#define REG_OCPWM_CTRL(base)	((base) + 0xC)

/* OCPWM_CTRL register bits*/
#define OCPWM_EN      BIT(0)
#define OCPWM_ECLK    BIT(1)
#define OCPWM_NEC     BIT(2)
#define OCPWM_OE      BIT(3)
#define OCPWM_SIGNLE  BIT(4)
#define OCPWM_INTE    BIT(5)
#define OCPWM_INT     BIT(6)
#define OCPWM_CNTRRST BIT(7)
#define OCPWM_CAPTE   BIT(8)

struct ocores_pwm_device {
	struct pwm_chip chip;
	struct clk *clk;
	struct reset_control *rst;
	const struct ocores_pwm_data *data;
	void __iomem *regs;
	u32 clk_rate; /* PWM APB clock frequency */
};

struct ocores_pwm_data {
	void __iomem *(*get_ch_base)(void __iomem *base, unsigned int channel);
};

static inline struct ocores_pwm_device *
chip_to_ocores(struct pwm_chip *chip)

{
	return container_of(chip, struct ocores_pwm_device, chip);
}

void __iomem *starfive_jh71x0_get_ch_base(void __iomem *base,
					  unsigned int channel)
{
	return base + (channel > 3 ? channel % 4 * 0x10 + (1 << 15) : channel * 0x10);
}

static int ocores_pwm_get_state(struct pwm_chip *chip,
				struct pwm_device *dev,
				struct pwm_state *state)
{
	struct ocores_pwm_device *pwm = chip_to_ocores(chip);
	void __iomem *base = pwm->data->get_ch_base ?
			     pwm->data->get_ch_base(pwm->regs, dev->hwpwm) : pwm->regs;
	u32 period_data, duty_data, ctrl_data;

	period_data = readl(REG_OCPWM_LRC(base));
	duty_data = readl(REG_OCPWM_HRC(base));
	ctrl_data = readl(REG_OCPWM_CTRL(base));

	state->period = DIV_ROUND_CLOSEST_ULL((u64)period_data * NSEC_PER_SEC, pwm->clk_rate);
	state->duty_cycle = DIV_ROUND_CLOSEST_ULL((u64)duty_data * NSEC_PER_SEC, pwm->clk_rate);
	state->polarity = PWM_POLARITY_INVERSED;
	state->enabled = (ctrl_data & OCPWM_EN) ? true : false;

	return 0;
}

static int ocores_pwm_apply(struct pwm_chip *chip,
			    struct pwm_device *dev,
			    const struct pwm_state *state)
{
	struct ocores_pwm_device *pwm = chip_to_ocores(chip);
	void __iomem *base = pwm->data->get_ch_base ?
			     pwm->data->get_ch_base(pwm->regs, dev->hwpwm) : pwm->regs;
	u32 period_data, duty_data, ctrl_data = 0;

	if (state->polarity != PWM_POLARITY_INVERSED)
		return -EINVAL;

	period_data = DIV_ROUND_CLOSEST_ULL(state->period * pwm->clk_rate,
					    NSEC_PER_SEC);
	duty_data = DIV_ROUND_CLOSEST_ULL(state->duty_cycle * pwm->clk_rate,
					  NSEC_PER_SEC);

	writel(period_data, REG_OCPWM_LRC(base));
	writel(duty_data, REG_OCPWM_HRC(base));
	writel(0,  REG_OCPWM_CNTR(base));

	ctrl_data = readl(REG_OCPWM_CTRL(base));
	if (state->enabled)
		writel(ctrl_data | OCPWM_EN | OCPWM_OE, REG_OCPWM_CTRL(base));
	else
		writel(ctrl_data & ~(OCPWM_EN | OCPWM_OE), REG_OCPWM_CTRL(base));

	return 0;
}

static const struct pwm_ops ocores_pwm_ops = {
	.get_state	= ocores_pwm_get_state,
	.apply		= ocores_pwm_apply,
};

static const struct ocores_pwm_data jh71x0_pwm_data = {
	.get_ch_base = starfive_jh71x0_get_ch_base,
};

static const struct of_device_id ocores_pwm_of_match[] = {
	{ .compatible = "opencores,pwm-ocores" },
	{ .compatible = "starfive,jh71x0-pwm", .data = &jh71x0_pwm_data},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ocores_pwm_of_match);

static int ocores_pwm_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	struct device *dev = &pdev->dev;
	struct ocores_pwm_device *pwm;
	struct pwm_chip *chip;
	int ret;

	id = of_match_device(ocores_pwm_of_match, dev);
	if (!id)
		return -EINVAL;

	pwm = devm_kzalloc(dev, sizeof(*pwm), GFP_KERNEL);
	if (!pwm)
		return -ENOMEM;

	pwm->data = id->data;
	chip = &pwm->chip;
	chip->dev = dev;
	chip->ops = &ocores_pwm_ops;
	chip->npwm = 8;
	chip->of_pwm_n_cells = 3;

	pwm->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pwm->regs))
		return dev_err_probe(dev, PTR_ERR(pwm->regs),
				     "Unable to map IO resources\n");

	pwm->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(pwm->clk))
		return dev_err_probe(dev, PTR_ERR(pwm->clk),
				     "Unable to get pwm's clock\n");

	pwm->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	reset_control_deassert(pwm->rst);

	pwm->clk_rate = clk_get_rate(pwm->clk);
	if (pwm->clk_rate <= 0) {
		dev_warn(dev, "Failed to get APB clock rate\n");
		return -EINVAL;
	}

	ret = devm_pwmchip_add(dev, chip);
	if (ret < 0) {
		dev_err(dev, "Cannot register PTC: %d\n", ret);
		clk_disable_unprepare(pwm->clk);
		reset_control_assert(pwm->rst);
		return ret;
	}

	platform_set_drvdata(pdev, pwm);

	return 0;
}

static int ocores_pwm_remove(struct platform_device *dev)
{
	struct ocores_pwm_device *pwm = platform_get_drvdata(dev);

	reset_control_assert(pwm->rst);
	clk_disable_unprepare(pwm->clk);

	return 0;
}

static struct platform_driver ocores_pwm_driver = {
	.probe = ocores_pwm_probe,
	.remove = ocores_pwm_remove,
	.driver = {
		.name = "ocores-pwm",
		.of_match_table = ocores_pwm_of_match,
	},
};
module_platform_driver(ocores_pwm_driver);

MODULE_AUTHOR("Jieqin Chen");
MODULE_AUTHOR("Hal Feng <hal.feng@starfivetech.com>");
MODULE_DESCRIPTION("OpenCores PWM PTC driver");
MODULE_LICENSE("GPL");
