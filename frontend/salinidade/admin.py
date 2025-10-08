# seu_app/admin.py
from django.contrib import admin
from .models import Sensor, Leitura

class LeituraInline(admin.TabularInline):
    model = Leitura
    extra = 0
    fields = ("data_hora", "tipo", "valor")
    ordering = ("-data_hora",)
    show_change_link = True

@admin.register(Sensor)
class SensorAdmin(admin.ModelAdmin):
    list_display = ("id", "nome", "descricao", "created_at", "updated_at")
    search_fields = ("nome", "descricao")
    ordering = ("id",)
    inlines = [LeituraInline]
    list_per_page = 25

@admin.register(Leitura)
class LeituraAdmin(admin.ModelAdmin):
    list_display = ("id", "sensor", "tipo", "valor", "data_hora")
    list_filter = ("tipo", "data_hora")
    search_fields = ("sensor__nome",)
    autocomplete_fields = ("sensor",)  # usa search_fields do SensorAdmin
    date_hierarchy = "data_hora"
    ordering = ("-data_hora",)
    list_per_page = 50

# (Opcional) Personalize títulos do Admin
admin.site.site_header = "Painel de Monitoramento"
admin.site.site_title = "Admin Monitoramento"
admin.site.index_title = "Bem-vindo"
