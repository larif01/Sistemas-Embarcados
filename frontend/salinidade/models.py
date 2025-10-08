from django.db import models
from django.utils import timezone


class Sensor(models.Model):
    id = models.BigAutoField(primary_key=True)
    nome = models.CharField("Nome", max_length=120)
    descricao = models.TextField("Descrição", blank=True, default="")

    created_at = models.DateTimeField("Criado em", auto_now_add=True)
    updated_at = models.DateTimeField("Atualizado em", auto_now=True)

    class Meta:
        db_table = "sensor"
        verbose_name = "Sensor"
        verbose_name_plural = "Sensores"
        ordering = ["id"]

    def __str__(self):
        return f"{self.nome} (ID {self.id})"


class Leitura(models.Model):
    class TipoLeitura(models.TextChoices):
        SALINIDADE = "SALI", "Salinidade"
        OUTRO        = "OUTR", "Outro"

    id = models.BigAutoField(primary_key=True)
    sensor = models.ForeignKey(
        Sensor,
        on_delete=models.CASCADE,
        related_name="leituras",
        db_index=True,
        verbose_name="Sensor (ID)"
    )

    valor = models.DecimalField("Valor obtido", max_digits=12, decimal_places=4)

    tipo = models.CharField(
        "Tipo da leitura",
        max_length=4,
        choices=TipoLeitura.choices,
        default=TipoLeitura.OUTRO,
    )

    data_hora = models.DateTimeField(
        "Data e hora da leitura",
        default=timezone.now,
        db_index=True
    )

    class Meta:
        db_table = "leitura"
        verbose_name = "Leitura"
        verbose_name_plural = "Leituras"
        ordering = ["-data_hora"]
        indexes = [
            models.Index(fields=["sensor", "data_hora"]),
        ]

    def __str__(self):
        return f"Leitura {self.id} - {self.get_tipo_display()} de {self.sensor.nome} em {self.data_hora:%Y-%m-%d %H:%M:%S}"
