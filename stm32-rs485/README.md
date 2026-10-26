# STM32 RS485 - maitre / esclave

Banc de test d'une liaison RS485 half-duplex entre deux cartes Nucleo-F446RE,
pilote depuis un PC par un script python.

## Principe

```
PC (python) --USART2--> MAITRE --UART4 / RS485--> ESCLAVE
PC (python) <--USART2-- MAITRE <--UART4 / RS485-- ESCLAVE
```

- Le **maitre** recoit des commandes texte du PC. Il envoie les trames a
  l'esclave, une par une, attend la reponse, puis la renvoie au PC.
  Il pilote aussi des sorties et lit des entrees (GPIO).
- L'**esclave** repond a chaque trame de 15 octets par une trame de 29 octets.
- Le **script python** execute des "fiches de test" (une liste de trames et
  les sorties a activer), decode les reponses et genere un PV de test (.txt).

## Liaison RS485

- Transceivers LTM2881, half-duplex
- 1 Mbit/s, 8 bits, parite paire, 1 bit de stop (8E1) sur les deux cartes
- Une broche GPIO (DE) commande le sens emission / reception
- Liaison PC <-> maitre : USART2 (ST-Link), 115200 bauds

## Contenu

| Dossier | Role |
|---|---|
| `maitre/` | Projet STM32CubeIDE de la carte maitre |
| `esclave/` | Projet STM32CubeIDE de la carte esclave |
| `script_python/` | Script de fiches de test et de generation du PV |

## Utilisation du script

```
pip install pyserial
python test_rs485_modbus.py
```

Adapter `PORT` (port COM du ST-Link) en haut du script, puis taper le nom
d'une fiche de test. `io <nom> <0|1>` pilote une sortie, `in` lit les entrees,
`q` quitte. Un PV de test `PV_<fiche>_<date>.txt` est ecrit apres chaque fiche.
